#if WITH_DEV_AUTOMATION_TESTS && WITH_METASOUND

#include "Misc/AutomationTest.h"
#include "Misc/ScopeExit.h"
#include "MonolithAudioOneShotBuilderContract.h"
#include "MetasoundBuilderSubsystem.h"
#include "Sound/SoundWave.h"

namespace
{
	FName MakeUniqueBuilderName(const TCHAR* TestName)
	{
		return FName(*FString::Printf(
			TEXT("%s_%s"),
			TestName,
			*FGuid::NewGuid().ToString(EGuidFormats::Digits)));
	}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FMonolithAudioOneShotBuilderUsesLiveMonoOutputMetadataTest,
	"Monolith.Audio.MetaSound.OneShotBuilderContract.UsesLiveMonoAudioOutputMetadata",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FMonolithAudioOneShotBuilderUsesLiveMonoOutputMetadataTest::RunTest(const FString& Parameters)
{
	UMetaSoundBuilderSubsystem& BuilderSubsystem = UMetaSoundBuilderSubsystem::GetChecked();
	const FName BuilderName = MakeUniqueBuilderName(TEXT("MonolithOneShotLiveMetadata"));
	ON_SCOPE_EXIT
	{
		BuilderSubsystem.UnregisterSourceBuilder(BuilderName);
	};

	FMetaSoundBuilderNodeOutputHandle OnPlayOutput;
	FMetaSoundBuilderNodeInputHandle OnFinishedInput;
	TArray<FMetaSoundBuilderNodeInputHandle> AudioOutInputs;
	EMetaSoundBuilderResult BuilderResult = EMetaSoundBuilderResult::Failed;
	UMetaSoundSourceBuilder* Builder = BuilderSubsystem.CreateSourceBuilder(
		BuilderName,
		OnPlayOutput,
		OnFinishedInput,
		AudioOutInputs,
		BuilderResult,
		EMetaSoundOutputAudioFormat::Mono,
		true);

	TestNotNull(TEXT("Transient mono source builder is created"), Builder);
	TestEqual(TEXT("Transient mono source builder creation succeeds"), BuilderResult, EMetaSoundBuilderResult::Succeeded);
	TestEqual(TEXT("Mono source builder exposes exactly one AudioOut input"), AudioOutInputs.Num(), 1);
	TestTrue(TEXT("Source builder remains in the transient package"), Builder && Builder->GetOutermost() == GetTransientPackage());
	TestFalse(TEXT("Source builder is not a persisted asset"), Builder && Builder->IsAsset());
	if (!Builder || BuilderResult != EMetaSoundBuilderResult::Succeeded || AudioOutInputs.Num() != 1)
	{
		return false;
	}

	USoundWave* TransientWave = NewObject<USoundWave>(GetTransientPackage(), NAME_None, RF_Transient);
	TestNotNull(TEXT("Transient test SoundWave is created"), TransientWave);
	TestFalse(TEXT("Test SoundWave is not a persisted asset"), TransientWave && TransientWave->IsAsset());
	if (!TransientWave)
	{
		return false;
	}

	MonolithAudio::OneShotBuilderContract::FMonoWavePlayerGraph Graph;
	FString Error;
	const bool bBuilt = MonolithAudio::OneShotBuilderContract::BuildMonoWavePlayerGraph(
		*Builder,
		*TransientWave,
		OnPlayOutput,
		OnFinishedInput,
		AudioOutInputs,
		Graph,
		Error);

	TestTrue(TEXT("One-shot graph builds from the engine's live Wave Player metadata"), bBuilt);
	TestTrue(TEXT("Successful graph build returns no error"), Error.IsEmpty());
	if (!bBuilt)
	{
		AddError(Error);
		return false;
	}

	EMetaSoundBuilderResult MetadataResult = EMetaSoundBuilderResult::Failed;
	const TArray<FMetaSoundBuilderNodeOutputHandle> TypedAudioOutputs = Builder->FindNodeOutputsByDataType(
		Graph.WavePlayerNode,
		MetadataResult,
		FName(TEXT("Audio")));
	TestEqual(TEXT("Live Wave Player audio metadata query succeeds"), MetadataResult, EMetaSoundBuilderResult::Succeeded);
	TestEqual(TEXT("Live mono Wave Player exposes exactly one Audio-typed output"), TypedAudioOutputs.Num(), 1);

	FName LiveOutputName;
	FName LiveOutputDataType;
	Builder->GetNodeOutputData(Graph.AudioOutput, LiveOutputName, LiveOutputDataType, MetadataResult);
	TestEqual(TEXT("Selected live output metadata query succeeds"), MetadataResult, EMetaSoundBuilderResult::Succeeded);
	TestEqual(TEXT("Selected output uses the Audio data type"), LiveOutputDataType, FName(TEXT("Audio")));
	TestFalse(TEXT("Selected output has a live engine-defined name"), LiveOutputName.IsNone());
	TestTrue(TEXT("Selected audio output is connected to the graph AudioOut input"), Builder->NodesAreConnected(Graph.AudioOutput, AudioOutInputs[0]));

	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FMonolithAudioOneShotBuilderMissingAudioOutFailsClosedTest,
	"Monolith.Audio.MetaSound.OneShotBuilderContract.MissingAudioOutInputFailsClosed",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FMonolithAudioOneShotBuilderMissingAudioOutFailsClosedTest::RunTest(const FString& Parameters)
{
	UMetaSoundBuilderSubsystem& BuilderSubsystem = UMetaSoundBuilderSubsystem::GetChecked();
	const FName BuilderName = MakeUniqueBuilderName(TEXT("MonolithOneShotMissingAudioOut"));
	ON_SCOPE_EXIT
	{
		BuilderSubsystem.UnregisterSourceBuilder(BuilderName);
	};

	FMetaSoundBuilderNodeOutputHandle OnPlayOutput;
	FMetaSoundBuilderNodeInputHandle OnFinishedInput;
	TArray<FMetaSoundBuilderNodeInputHandle> BuilderAudioOutInputs;
	EMetaSoundBuilderResult BuilderResult = EMetaSoundBuilderResult::Failed;
	UMetaSoundSourceBuilder* Builder = BuilderSubsystem.CreateSourceBuilder(
		BuilderName,
		OnPlayOutput,
		OnFinishedInput,
		BuilderAudioOutInputs,
		BuilderResult,
		EMetaSoundOutputAudioFormat::Mono,
		true);

	TestNotNull(TEXT("Transient mono source builder is created"), Builder);
	TestEqual(TEXT("Transient mono source builder creation succeeds"), BuilderResult, EMetaSoundBuilderResult::Succeeded);
	TestEqual(TEXT("Control builder has one real AudioOut input"), BuilderAudioOutInputs.Num(), 1);
	TestTrue(TEXT("Source builder remains in the transient package"), Builder && Builder->GetOutermost() == GetTransientPackage());
	TestFalse(TEXT("Source builder is not a persisted asset"), Builder && Builder->IsAsset());
	if (!Builder || BuilderResult != EMetaSoundBuilderResult::Succeeded)
	{
		return false;
	}

	USoundWave* TransientWave = NewObject<USoundWave>(GetTransientPackage(), NAME_None, RF_Transient);
	TestNotNull(TEXT("Transient test SoundWave is created"), TransientWave);
	TestFalse(TEXT("Test SoundWave is not a persisted asset"), TransientWave && TransientWave->IsAsset());
	if (!TransientWave)
	{
		return false;
	}

	const TArray<FMetaSoundBuilderNodeInputHandle> MissingAudioOutInputs;
	MonolithAudio::OneShotBuilderContract::FMonoWavePlayerGraph Graph;
	FString Error;
	const bool bBuilt = MonolithAudio::OneShotBuilderContract::BuildMonoWavePlayerGraph(
		*Builder,
		*TransientWave,
		OnPlayOutput,
		OnFinishedInput,
		MissingAudioOutInputs,
		Graph,
		Error);

	TestFalse(TEXT("One-shot graph build fails closed without an AudioOut input"), bBuilt);
	TestEqual(
		TEXT("Failure identifies the exact violated AudioOut contract"),
		Error,
		FString(TEXT("create_oneshot_sfx graph contract requires exactly one Mono AudioOut input; received 0.")));
	TestFalse(TEXT("Failure returns no partially accepted Wave Player node"), Builder->ContainsNode(Graph.WavePlayerNode));
	TestFalse(TEXT("Failure returns no partially accepted audio output"), Builder->ContainsNodeOutput(Graph.AudioOutput));

	return true;
}

#endif // WITH_DEV_AUTOMATION_TESTS && WITH_METASOUND
