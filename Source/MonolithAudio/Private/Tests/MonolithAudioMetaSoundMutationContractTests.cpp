#if WITH_DEV_AUTOMATION_TESTS && WITH_METASOUND

#include "Misc/AutomationTest.h"
#include "Misc/ScopeExit.h"
#include "MonolithAudioMetaSoundMutationContract.h"
#include "MonolithAudioOneShotBuilderContract.h"
#include "MetasoundBuilderSubsystem.h"
#include "Sound/SoundWave.h"

namespace
{
	FName MakeMutationBuilderName(const TCHAR* TestName)
	{
		return FName(*FString::Printf(
			TEXT("%s_%s"),
			TestName,
			*FGuid::NewGuid().ToString(EGuidFormats::Digits)));
	}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FMonolithAudioMetaSoundMutationConnectsAndVerifiesNamedPinsTest,
	"Monolith.Audio.MetaSound.MutationContract.ConnectsAndVerifiesExactNamedPins",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FMonolithAudioMetaSoundMutationConnectsAndVerifiesNamedPinsTest::RunTest(const FString& Parameters)
{
	UMetaSoundBuilderSubsystem& BuilderSubsystem = UMetaSoundBuilderSubsystem::GetChecked();
	const FName BuilderName = MakeMutationBuilderName(TEXT("MonolithMutationConnect"));
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
	if (!Builder || BuilderResult != EMetaSoundBuilderResult::Succeeded || AudioOutInputs.Num() != 1)
	{
		return false;
	}

	USoundWave* TransientWave = NewObject<USoundWave>(GetTransientPackage(), NAME_None, RF_Transient);
	TestNotNull(TEXT("Transient test SoundWave is created"), TransientWave);
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
	TestTrue(TEXT("Control one-shot graph builds"), bBuilt);
	if (!bBuilt)
	{
		AddError(Error);
		return false;
	}

	FName LiveOutputName;
	FName LiveOutputDataType;
	Builder->GetNodeOutputData(Graph.AudioOutput, LiveOutputName, LiveOutputDataType, BuilderResult);
	TestEqual(TEXT("Wave Player output metadata lookup succeeds"), BuilderResult, EMetaSoundBuilderResult::Succeeded);
	TestEqual(TEXT("Wave Player output is Audio typed"), LiveOutputDataType, FName(TEXT("Audio")));

	FName LiveInputName;
	FName LiveInputDataType;
	Builder->GetNodeInputData(AudioOutInputs[0], LiveInputName, LiveInputDataType, BuilderResult);
	TestEqual(TEXT("AudioOut input metadata lookup succeeds"), BuilderResult, EMetaSoundBuilderResult::Succeeded);
	TestEqual(TEXT("AudioOut input is Audio typed"), LiveInputDataType, FName(TEXT("Audio")));

	Builder->DisconnectNodes(Graph.AudioOutput, AudioOutInputs[0], BuilderResult);
	TestEqual(TEXT("Control audio edge is disconnected"), BuilderResult, EMetaSoundBuilderResult::Succeeded);
	TestFalse(TEXT("Control graph starts without the audio edge"), Builder->NodesAreConnected(Graph.AudioOutput, AudioOutInputs[0]));

	FMetaSoundNodeHandle AudioOutNode;
	AudioOutNode.NodeID = AudioOutInputs[0].NodeID;
	MonolithAudio::MetaSoundMutationContract::FNamedConnection Connection;
	const bool bConnected = MonolithAudio::MetaSoundMutationContract::ConnectNamedPinsAndVerify(
		*Builder,
		Graph.WavePlayerNode,
		LiveOutputName,
		AudioOutNode,
		LiveInputName,
		Connection,
		Error);

	TestTrue(TEXT("Exact live pin names connect successfully"), bConnected);
	TestTrue(TEXT("Successful connection reports no error"), Error.IsEmpty());
	TestFalse(TEXT("First connection reports a topology mutation"), Connection.bAlreadyConnected);
	TestTrue(TEXT("Connection contract verifies the final edge"), Builder->NodesAreConnected(Connection.Output, Connection.Input));

	MonolithAudio::MetaSoundMutationContract::FNamedConnection RepeatedConnection;
	const bool bRepeated = MonolithAudio::MetaSoundMutationContract::ConnectNamedPinsAndVerify(
		*Builder,
		Graph.WavePlayerNode,
		LiveOutputName,
		AudioOutNode,
		LiveInputName,
		RepeatedConnection,
		Error);
	TestTrue(TEXT("Repeated exact connection is idempotent"), bRepeated);
	TestTrue(TEXT("Repeated connection reports the existing edge"), RepeatedConnection.bAlreadyConnected);

	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FMonolithAudioMetaSoundMutationRejectsTransientPersistenceTest,
	"Monolith.Audio.MetaSound.MutationContract.TransientBuilderPersistenceFailsClosed",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FMonolithAudioMetaSoundMutationRejectsTransientPersistenceTest::RunTest(const FString& Parameters)
{
	UMetaSoundBuilderSubsystem& BuilderSubsystem = UMetaSoundBuilderSubsystem::GetChecked();
	const FName BuilderName = MakeMutationBuilderName(TEXT("MonolithMutationTransientSave"));
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
	if (!Builder || BuilderResult != EMetaSoundBuilderResult::Succeeded)
	{
		return false;
	}

	MonolithAudio::MetaSoundMutationContract::FPersistedAsset PersistedAsset;
	FString Error;
	const bool bSaved = MonolithAudio::MetaSoundMutationContract::PersistAttachedAsset(
		*Builder,
		PersistedAsset,
		Error);

	TestFalse(TEXT("Transient builder cannot be reported as persisted"), bSaved);
	TestTrue(TEXT("Transient persistence failure identifies the missing persistent package"), Error.Contains(TEXT("not attached to a persistent asset package")));
	TestTrue(TEXT("Failed persistence returns no asset path"), PersistedAsset.AssetPath.IsEmpty());
	TestTrue(TEXT("Failed persistence returns no package name"), PersistedAsset.PackageName.IsEmpty());
	TestTrue(TEXT("Failed persistence returns no filename"), PersistedAsset.Filename.IsEmpty());

	return true;
}

#endif // WITH_DEV_AUTOMATION_TESTS && WITH_METASOUND
