#if 0
void GameplayMessageTraceFixture()
{
	MessageSubsystem.BroadcastMessage<FMonolithGameplayMessageFixturePayload>(TAG_Monolith_GameplayMessage_Fixture, Payload);
	MessageSubsystem.RegisterListener<FMonolithGameplayMessageFixturePayload>(TAG_Monolith_GameplayMessage_Fixture, Callback, EGameplayMessageMatch::ExactMatch);
}
#endif
