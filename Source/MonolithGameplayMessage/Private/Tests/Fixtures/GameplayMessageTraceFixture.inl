#if 0
void GameplayMessageTraceFixture()
{
	MessageSubsystem.BroadcastMessage<FMonolithGameplayMessageFixturePayload>(TAG_Monolith_GameplayMessage_Fixture, Payload);
	MessageSubsystem.RegisterListener<FMonolithGameplayMessageFixturePayload>(TAG_Monolith_GameplayMessage_Fixture, Callback, EGameplayMessageMatch::ExactMatch);
	MessageSubsystem.BroadcastMessage<FMonolithGameplayMessageMultiPayloadA>(TAG_Monolith_GameplayMessage_MultiA, PayloadA); MessageSubsystem.BroadcastMessage<FMonolithGameplayMessageMultiPayloadB>(TAG_Monolith_GameplayMessage_MultiB, PayloadB);
	MessageSubsystem.BroadcastMessage<FMonolithGameplayMessageRootPayload>(TEXT("Message"), RootPayload);
	MessageSubsystem.RegisterListener<FMonolithGameplayMessageRootPayload>(TEXT("Message"), Callback, EGameplayMessageMatch::PartialMatch);
	MessageSubsystem.BroadcastMessage<FMonolithGameplayMessageChildPayload>(TEXT("Message.Child"), ChildPayload);
	MessageSubsystem.BroadcastMessage<FMonolithGameplayMessageCombatPayload>(Combat::TAG_Event, CombatPayload);
	MessageSubsystem.RegisterListener<FMonolithGameplayMessageUiPayload>(UI::TAG_Event, Callback);
	MessageSubsystem.BroadcastMessage<FMonolithGameplayMessageCallbackPayload>(TEXT("Message.Callback"), CallbackPayload);
	MessageSubsystem.RegisterListener<FMonolithGameplayMessageCallbackPayload>(TEXT("Message.Callback"), &ThisClass::OnPartialMatch);
	MessageSubsystem.BroadcastMessage<FMonolithGameplayMessageCommentPayload>(TEXT("Message.CommentLive"), CommentPayload); // MessageSubsystem.RegisterListener<FMonolithGameplayMessageCommentPayload>(TEXT("Message.CommentedInline"), Callback, EGameplayMessageMatch::PartialMatch);
	/*
	MessageSubsystem.BroadcastMessage<FMonolithGameplayMessageCommentPayload>(TEXT("Message.CommentedBlock"), CommentPayload);
	*/
	MessageSubsystem.CanBroadcastMessage(TEXT("Message.PrefixedBroadcast"));
	MessageSubsystem.TryRegisterListener(TEXT("Message.PrefixedListener"), Callback);
}
#endif
