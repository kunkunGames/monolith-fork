#if 0
void GameplayMessageTraceFixture()
{
	MessageSubsystem.BroadcastMessage<FMonolithGameplayMessageFixturePayload>(TAG_Monolith_GameplayMessage_Fixture, Payload);
	MessageSubsystem.RegisterListener<FMonolithGameplayMessageFixturePayload>(TAG_Monolith_GameplayMessage_Fixture, Callback, EGameplayMessageMatch::ExactMatch);
	MessageSubsystem.BroadcastMessage<FMonolithGameplayMessageMultiPayloadA>(TAG_Monolith_GameplayMessage_MultiA, PayloadA); MessageSubsystem.BroadcastMessage<FMonolithGameplayMessageMultiPayloadB>(TAG_Monolith_GameplayMessage_MultiB, PayloadB);
	MessageSubsystem.BroadcastMessage<FMonolithGameplayMessageRootPayload>(TEXT("Message"), RootPayload);
}
#endif
