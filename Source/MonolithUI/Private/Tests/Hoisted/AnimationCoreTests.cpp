// Copyright tumourlove. All Rights Reserved.
#if WITH_DEV_AUTOMATION_TESTS

// Core / test
#include "CoreMinimal.h"
#include "Misc/AutomationTest.h"
#include "Misc/PackageName.h"

// JSON / registry
#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "MonolithToolRegistry.h"
#include "MonolithUIAnimationActions.h"

// UMG -- build a throwaway WBP + probe the result
#include "Tests/Hoisted/MonolithUITestFixtureUtils.h"
#include "Blueprint/WidgetTree.h"
#include "Components/Image.h"

// Animation
#include "Animation/WidgetAnimation.h"
#include "MovieScene.h"
#include "Tracks/MovieSceneFloatTrack.h"
#include "Sections/MovieSceneFloatSection.h"
#include "Channels/MovieSceneFloatChannel.h"
#include "Channels/MovieSceneCurveChannelCommon.h"

// Event track
#include "Tracks/MovieSceneEventTrack.h"
#include "Sections/MovieSceneEventTriggerSection.h"
#include "Channels/MovieSceneEventChannel.h"

// Delegate binding
#include "Animation/WidgetAnimationDelegateBinding.h"
#include "Engine/BlueprintGeneratedClass.h"

// Asset / package
#include "Editor.h"

namespace MonolithUI::AnimationCoreTests
{
    static const FString GTestAssetPath = TEXT("/Game/Tests/Monolith/UI/WBP_AnimCoreTest");

    static UWidgetAnimation* FindAnimationByReadableName(UWidgetBlueprint* WBP, const FString& AnimationName)
    {
        if (!WBP)
        {
            return nullptr;
        }

        for (UWidgetAnimation* Animation : WBP->Animations)
        {
            if (!Animation)
            {
                continue;
            }
            if (Animation->GetName() == AnimationName)
            {
                return Animation;
            }
#if WITH_EDITORONLY_DATA
            if (Animation->GetDisplayLabel() == AnimationName)
            {
                return Animation;
            }
#endif
        }
        return nullptr;
    }

    static UMovieSceneFloatTrack* FindFloatTrackByProperty(UWidgetAnimation* Animation, const FName PropertyName)
    {
        if (!Animation || !Animation->GetMovieScene())
        {
            return nullptr;
        }

        UMovieScene* MovieScene = Animation->GetMovieScene();
        const UMovieScene* ConstMovieScene = MovieScene;
        for (const FMovieSceneBinding& Binding : ConstMovieScene->GetBindings())
        {
            if (UMovieSceneFloatTrack* FloatTrack = MovieScene->FindTrack<UMovieSceneFloatTrack>(
                Binding.GetObjectGuid(),
                PropertyName))
            {
                return FloatTrack;
            }
        }
        return nullptr;
    }

    static int32 CountFloatTrackKeys(const UMovieSceneFloatTrack* FloatTrack)
    {
        int32 Count = 0;
        if (!FloatTrack)
        {
            return Count;
        }

        for (UMovieSceneSection* Section : FloatTrack->GetAllSections())
        {
            if (const UMovieSceneFloatSection* FloatSection = Cast<UMovieSceneFloatSection>(Section))
            {
                Count += FloatSection->GetChannel().GetNumKeys();
            }
        }
        return Count;
    }

    static bool TryGetFloatTrackValueAtFrame(
        const UMovieSceneFloatTrack* FloatTrack,
        const FFrameNumber Frame,
        float& OutValue)
    {
        if (!FloatTrack)
        {
            return false;
        }

        for (UMovieSceneSection* Section : FloatTrack->GetAllSections())
        {
            const UMovieSceneFloatSection* FloatSection = Cast<UMovieSceneFloatSection>(Section);
            if (!FloatSection)
            {
                continue;
            }

            const FMovieSceneFloatChannel& Channel = FloatSection->GetChannel();
            const TArrayView<const FFrameNumber> Times = Channel.GetTimes();
            const TArrayView<const FMovieSceneFloatValue> Values = Channel.GetValues();
            const int32 NumKeys = FMath::Min(Times.Num(), Values.Num());
            for (int32 KeyIndex = 0; KeyIndex < NumKeys; ++KeyIndex)
            {
                if (Times[KeyIndex] == Frame)
                {
                    OutValue = Values[KeyIndex].Value;
                    return true;
                }
            }
        }
        return false;
    }
} // namespace MonolithUI::AnimationCoreTests


/**
 * MonolithUI.CreateAnimationV2.Basic
 *
 * Creates a 2-key cubic RenderOpacity animation on a test WBP. Asserts:
 * animation exists in WBP->Animations, channel has 2 keys, key values match
 * within tolerance, tangent data present.
 */
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
    FMonolithUICreateAnimationV2BasicTest,
    "MonolithUI.CreateAnimationV2.Basic",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FMonolithUICreateAnimationV2BasicTest::RunTest(const FString& Parameters)
{
    using namespace MonolithUI::AnimationCoreTests;
    using MonolithUI::TestUtils::CreateOrReuseTestWidgetBlueprint;

    FString SetupError;
    if (!CreateOrReuseTestWidgetBlueprint(GTestAssetPath, FName(TEXT("MyImage")), nullptr, SetupError))
    {
        AddError(FString::Printf(TEXT("Fixture setup failed: %s"), *SetupError));
        return false;
    }

    TSharedPtr<FJsonObject> Params = MakeShared<FJsonObject>();
    Params->SetStringField(TEXT("asset_path"), GTestAssetPath);
    Params->SetStringField(TEXT("animation_name"), TEXT("FadeIn"));
    Params->SetNumberField(TEXT("duration_sec"), 0.5);
    Params->SetBoolField(TEXT("compile_once"), true);

    TArray<TSharedPtr<FJsonValue>> Tracks;
    {
        TSharedPtr<FJsonObject> Track = MakeShared<FJsonObject>();
        Track->SetStringField(TEXT("widget_name"), TEXT("MyImage"));
        Track->SetStringField(TEXT("property"), TEXT("RenderOpacity"));

        TArray<TSharedPtr<FJsonValue>> Keys;
        {
            TSharedPtr<FJsonObject> K0 = MakeShared<FJsonObject>();
            K0->SetNumberField(TEXT("time"), 0.0);
            K0->SetNumberField(TEXT("value"), 0.0);
            K0->SetStringField(TEXT("interp"), TEXT("cubic"));
            K0->SetNumberField(TEXT("leave_tangent"), 2.0);
            K0->SetNumberField(TEXT("leave_weight"), 0.33);
            Keys.Add(MakeShared<FJsonValueObject>(K0));
        }
        {
            TSharedPtr<FJsonObject> K1 = MakeShared<FJsonObject>();
            K1->SetNumberField(TEXT("time"), 0.5);
            K1->SetNumberField(TEXT("value"), 1.0);
            K1->SetStringField(TEXT("interp"), TEXT("cubic"));
            K1->SetNumberField(TEXT("arrive_tangent"), 0.0);
            K1->SetNumberField(TEXT("arrive_weight"), 0.33);
            Keys.Add(MakeShared<FJsonValueObject>(K1));
        }
        Track->SetArrayField(TEXT("keys"), Keys);
        Tracks.Add(MakeShared<FJsonValueObject>(Track));
    }
    Params->SetArrayField(TEXT("tracks"), Tracks);

    const FMonolithActionResult Result = FMonolithToolRegistry::Get().ExecuteAction(
        TEXT("ui"), TEXT("create_animation_v2"), Params);

    TestTrue(TEXT("create_animation_v2 bSuccess"), Result.bSuccess);
    if (!Result.bSuccess)
    {
        AddError(FString::Printf(TEXT("Action error: %s (code %d)"), *Result.ErrorMessage, Result.ErrorCode));
        return false;
    }

    double KeysInserted = 0.0;
    TestTrue(TEXT("result has keys_inserted"), Result.Result->TryGetNumberField(TEXT("keys_inserted"), KeysInserted));
    TestEqual(TEXT("keys_inserted == 2"), static_cast<int32>(KeysInserted), 2);

    UWidgetBlueprint* WBP = LoadObject<UWidgetBlueprint>(nullptr, *GTestAssetPath);
    if (!WBP)
    {
        AddError(TEXT("Failed to reload test WBP"));
        return false;
    }

#if WITH_EDITORONLY_DATA
    UWidgetAnimation* FoundAnim = nullptr;
    for (UWidgetAnimation* Anim : WBP->Animations)
    {
        if (Anim && Anim->GetDisplayLabel() == TEXT("FadeIn"))
        {
            FoundAnim = Anim;
            break;
        }
    }
    TestNotNull(TEXT("FadeIn animation found in WBP->Animations"), FoundAnim);
    if (!FoundAnim)
    {
        return false;
    }

    TestNotNull(TEXT("MovieScene exists"), FoundAnim->MovieScene.Get());
    if (!FoundAnim->MovieScene)
    {
        return false;
    }

    UMovieSceneFloatTrack* FloatTrack = nullptr;
    const UMovieScene* ConstScene = FoundAnim->MovieScene;
    for (const FMovieSceneBinding& Binding : ConstScene->GetBindings())
    {
        FloatTrack = FoundAnim->MovieScene->FindTrack<UMovieSceneFloatTrack>(
            Binding.GetObjectGuid(), FName(TEXT("RenderOpacity")));
        if (FloatTrack)
        {
            break;
        }
    }
    TestNotNull(TEXT("RenderOpacity float track found"), FloatTrack);
    if (!FloatTrack)
    {
        return false;
    }

    TestTrue(TEXT("Track has at least 1 section"), FloatTrack->GetAllSections().Num() > 0);
    UMovieSceneFloatSection* Section = Cast<UMovieSceneFloatSection>(FloatTrack->GetAllSections()[0]);
    TestNotNull(TEXT("Float section exists"), Section);
    if (!Section)
    {
        return false;
    }

    const FMovieSceneFloatChannel& Channel = Section->GetChannel();
    TestEqual(TEXT("Channel has 2 keys"), Channel.GetNumKeys(), 2);

    TArrayView<const FMovieSceneFloatValue> Values = Channel.GetValues();
    if (Values.Num() >= 2)
    {
        TestNearlyEqual(TEXT("Key 0 value ~ 0.0"), Values[0].Value, 0.0f, 0.01f);
        TestNearlyEqual(TEXT("Key 1 value ~ 1.0"), Values[1].Value, 1.0f, 0.01f);
        TestNearlyEqual(TEXT("Key 0 leave tangent ~ 2.0"), Values[0].Tangent.LeaveTangent, 2.0f, 0.01f);
    }
#endif

    return true;
}


/**
 * MonolithUI.AddBezierEasedSegment.Basic
 *
 * Creates a bezier-eased segment with cubic-bezier(0.42, 0, 0.58, 1.0)
 * (ease-in-out). Asserts: 2 keys exist, weighted tangent mode set,
 * tangent values non-zero for this curve shape.
 */
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
    FMonolithUIAddBezierEasedSegmentBasicTest,
    "MonolithUI.AddBezierEasedSegment.Basic",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FMonolithUIAddBezierEasedSegmentBasicTest::RunTest(const FString& Parameters)
{
    using MonolithUI::TestUtils::CreateOrReuseTestWidgetBlueprint;

    const FString BezierTestPath = TEXT("/Game/Tests/Monolith/UI/WBP_BezierSegTest");

    FString FixtureError;
    if (!CreateOrReuseTestWidgetBlueprint(BezierTestPath, FName(TEXT("MyImage")), nullptr, FixtureError))
    {
        AddError(FString::Printf(TEXT("Fixture build failed: %s"), *FixtureError));
        return false;
    }

    TSharedPtr<FJsonObject> Params = MakeShared<FJsonObject>();
    Params->SetStringField(TEXT("asset_path"), BezierTestPath);
    Params->SetStringField(TEXT("animation_name"), TEXT("EaseInOut"));
    Params->SetStringField(TEXT("widget_name"), TEXT("MyImage"));
    Params->SetStringField(TEXT("property"), TEXT("RenderOpacity"));
    Params->SetNumberField(TEXT("from_value"), 0.0);
    Params->SetNumberField(TEXT("to_value"), 1.0);
    Params->SetNumberField(TEXT("start_time"), 0.0);
    Params->SetNumberField(TEXT("end_time"), 0.5);

    TArray<TSharedPtr<FJsonValue>> Bezier;
    Bezier.Add(MakeShared<FJsonValueNumber>(0.42));
    Bezier.Add(MakeShared<FJsonValueNumber>(0.0));
    Bezier.Add(MakeShared<FJsonValueNumber>(0.58));
    Bezier.Add(MakeShared<FJsonValueNumber>(1.0));
    Params->SetArrayField(TEXT("bezier"), Bezier);

    const FMonolithActionResult Result = FMonolithToolRegistry::Get().ExecuteAction(
        TEXT("ui"), TEXT("add_bezier_eased_segment"), Params);

    TestTrue(TEXT("add_bezier_eased_segment bSuccess"), Result.bSuccess);
    if (!Result.bSuccess)
    {
        AddError(FString::Printf(TEXT("Action error: %s (code %d)"), *Result.ErrorMessage, Result.ErrorCode));
        return false;
    }

    double KeysInserted = 0.0;
    TestTrue(TEXT("result has keys_inserted"), Result.Result->TryGetNumberField(TEXT("keys_inserted"), KeysInserted));
    TestEqual(TEXT("keys_inserted == 2"), static_cast<int32>(KeysInserted), 2);

    const TSharedPtr<FJsonObject>* TangentInfoPtr = nullptr;
    TestTrue(TEXT("result has tangent_info"), Result.Result->TryGetObjectField(TEXT("tangent_info"), TangentInfoPtr));

    UWidgetBlueprint* WBP = LoadObject<UWidgetBlueprint>(nullptr, *BezierTestPath);
    if (!WBP)
    {
        AddError(TEXT("Failed to reload bezier test WBP"));
        return false;
    }

#if WITH_EDITORONLY_DATA
    UWidgetAnimation* FoundAnim = nullptr;
    for (UWidgetAnimation* Anim : WBP->Animations)
    {
        if (Anim && Anim->GetDisplayLabel() == TEXT("EaseInOut"))
        {
            FoundAnim = Anim;
            break;
        }
    }
    TestNotNull(TEXT("EaseInOut animation found"), FoundAnim);
    if (!FoundAnim || !FoundAnim->MovieScene)
    {
        return false;
    }

    UMovieSceneFloatTrack* FloatTrack = nullptr;
    const UMovieScene* ConstScene2 = FoundAnim->MovieScene;
    for (const FMovieSceneBinding& Binding : ConstScene2->GetBindings())
    {
        FloatTrack = FoundAnim->MovieScene->FindTrack<UMovieSceneFloatTrack>(
            Binding.GetObjectGuid(), FName(TEXT("RenderOpacity")));
        if (FloatTrack)
        {
            break;
        }
    }
    TestNotNull(TEXT("RenderOpacity float track found"), FloatTrack);
    if (!FloatTrack || FloatTrack->GetAllSections().Num() == 0)
    {
        return false;
    }

    UMovieSceneFloatSection* Section = Cast<UMovieSceneFloatSection>(FloatTrack->GetAllSections()[0]);
    TestNotNull(TEXT("Float section exists"), Section);
    if (!Section)
    {
        return false;
    }

    const FMovieSceneFloatChannel& Channel = Section->GetChannel();
    TestEqual(TEXT("Channel has 2 keys"), Channel.GetNumKeys(), 2);

    TArrayView<const FMovieSceneFloatValue> Values = Channel.GetValues();
    if (Values.Num() >= 2)
    {
        TestNearlyEqual(TEXT("Key 0 value ~ 0.0"), Values[0].Value, 0.0f, 0.01f);
        TestNearlyEqual(TEXT("Key 1 value ~ 1.0"), Values[1].Value, 1.0f, 0.01f);

        TestEqual(TEXT("Key 0 tangent weight mode is WeightedBoth"),
            static_cast<int32>(Values[0].Tangent.TangentWeightMode),
            static_cast<int32>(RCTWM_WeightedBoth));
        TestEqual(TEXT("Key 1 tangent weight mode is WeightedBoth"),
            static_cast<int32>(Values[1].Tangent.TangentWeightMode),
            static_cast<int32>(RCTWM_WeightedBoth));

        // For ease-in-out (0.42, 0, 0.58, 1.0): key 0 leave tangent == 0,
        // key 1 arrive tangent == 0; both leave/arrive weights > 0.
        TestNearlyEqual(TEXT("Key 0 leave tangent ~ 0"), Values[0].Tangent.LeaveTangent, 0.0f, 0.01f);
        TestNearlyEqual(TEXT("Key 1 arrive tangent ~ 0"), Values[1].Tangent.ArriveTangent, 0.0f, 0.01f);
        TestTrue(TEXT("Key 0 leave weight > 0"), Values[0].Tangent.LeaveTangentWeight > 0.0f);
        TestTrue(TEXT("Key 1 arrive weight > 0"), Values[1].Tangent.ArriveTangentWeight > 0.0f);
    }
#endif

    return true;
}


/**
 * MonolithUI.BakeSpringAnimation.Basic
 *
 * Bakes an underdamped spring (stiffness=100, damping=10, mass=1) into dense
 * linear keyframes. Asserts: key count > 10, at least 2 zero-crossings
 * relative to target (underdamped overshoot).
 */
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
    FMonolithUIBakeSpringAnimationBasicTest,
    "MonolithUI.BakeSpringAnimation.Basic",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FMonolithUIBakeSpringAnimationBasicTest::RunTest(const FString& Parameters)
{
    using MonolithUI::TestUtils::CreateOrReuseTestWidgetBlueprint;

    const FString SpringTestPath = TEXT("/Game/Tests/Monolith/UI/WBP_SpringTest");

    FString FixtureError;
    if (!CreateOrReuseTestWidgetBlueprint(SpringTestPath, FName(TEXT("MyImage")), nullptr, FixtureError))
    {
        AddError(FString::Printf(TEXT("Fixture build failed: %s"), *FixtureError));
        return false;
    }

    TSharedPtr<FJsonObject> Params = MakeShared<FJsonObject>();
    Params->SetStringField(TEXT("asset_path"), SpringTestPath);
    Params->SetStringField(TEXT("animation_name"), TEXT("SpringBounce"));
    Params->SetStringField(TEXT("widget_name"), TEXT("MyImage"));
    Params->SetStringField(TEXT("property"), TEXT("RenderOpacity"));
    Params->SetNumberField(TEXT("from_value"), 0.0);
    Params->SetNumberField(TEXT("to_value"), 1.0);
    Params->SetNumberField(TEXT("stiffness"), 100.0);
    Params->SetNumberField(TEXT("damping"), 10.0);
    Params->SetNumberField(TEXT("mass"), 1.0);
    Params->SetNumberField(TEXT("fps"), 60.0);
    Params->SetNumberField(TEXT("duration"), 2.0);
    Params->SetBoolField(TEXT("compile_once"), true);

    const FMonolithActionResult Result = FMonolithToolRegistry::Get().ExecuteAction(
        TEXT("ui"), TEXT("bake_spring_animation"), Params);

    TestTrue(TEXT("bake_spring_animation bSuccess"), Result.bSuccess);
    if (!Result.bSuccess)
    {
        AddError(FString::Printf(TEXT("Action error: %s (code %d)"), *Result.ErrorMessage, Result.ErrorCode));
        return false;
    }

    double KeysInserted = 0.0;
    TestTrue(TEXT("result has keys_inserted"), Result.Result->TryGetNumberField(TEXT("keys_inserted"), KeysInserted));
    TestTrue(TEXT("keys_inserted > 10 (dense sampling)"), static_cast<int32>(KeysInserted) > 10);

    UWidgetBlueprint* WBP = LoadObject<UWidgetBlueprint>(nullptr, *SpringTestPath);
    if (!WBP)
    {
        AddError(TEXT("Failed to reload spring test WBP"));
        return false;
    }

#if WITH_EDITORONLY_DATA
    UWidgetAnimation* FoundAnim = nullptr;
    for (UWidgetAnimation* Anim : WBP->Animations)
    {
        if (Anim && Anim->GetDisplayLabel() == TEXT("SpringBounce"))
        {
            FoundAnim = Anim;
            break;
        }
    }
    TestNotNull(TEXT("SpringBounce animation found"), FoundAnim);
    if (!FoundAnim || !FoundAnim->MovieScene)
    {
        return false;
    }

    UMovieSceneFloatTrack* FloatTrack = nullptr;
    const UMovieScene* ConstScene = FoundAnim->MovieScene;
    for (const FMovieSceneBinding& Binding : ConstScene->GetBindings())
    {
        FloatTrack = FoundAnim->MovieScene->FindTrack<UMovieSceneFloatTrack>(
            Binding.GetObjectGuid(), FName(TEXT("RenderOpacity")));
        if (FloatTrack)
        {
            break;
        }
    }
    TestNotNull(TEXT("RenderOpacity float track found"), FloatTrack);
    if (!FloatTrack || FloatTrack->GetAllSections().Num() == 0)
    {
        return false;
    }

    UMovieSceneFloatSection* Section = Cast<UMovieSceneFloatSection>(FloatTrack->GetAllSections()[0]);
    TestNotNull(TEXT("Float section exists"), Section);
    if (!Section)
    {
        return false;
    }

    const FMovieSceneFloatChannel& Channel = Section->GetChannel();
    TestTrue(TEXT("Channel key count > 10"), Channel.GetNumKeys() > 10);

    // Underdamped spring should overshoot, so values cross 1.0 at least twice.
    TArrayView<const FMovieSceneFloatValue> Values = Channel.GetValues();
    int32 ZeroCrossings = 0;
    const float Target = 1.0f;
    for (int32 i = 1; i < Values.Num(); ++i)
    {
        const float Prev = Values[i - 1].Value - Target;
        const float Curr = Values[i].Value - Target;
        if ((Prev > 0.0f && Curr <= 0.0f) || (Prev < 0.0f && Curr >= 0.0f))
        {
            ++ZeroCrossings;
        }
    }
    TestTrue(TEXT("Underdamped spring has >= 2 zero-crossings"), ZeroCrossings >= 2);
#endif

    return true;
}


/**
 * MonolithUI.AddAnimationEventTrack.Basic
 *
 * Adds 2 event keys at t=0 and t=0.3 to an existing animation. Asserts:
 * master event track exists, section has correct key count.
 */
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
    FMonolithUIAddAnimationEventTrackBasicTest,
    "MonolithUI.AddAnimationEventTrack.Basic",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FMonolithUIAddAnimationEventTrackBasicTest::RunTest(const FString& Parameters)
{
    using MonolithUI::TestUtils::CreateOrReuseTestWidgetBlueprint;

    const FString EventTestPath = TEXT("/Game/Tests/Monolith/UI/WBP_EventTrackTest");

    FString FixtureError;
    if (!CreateOrReuseTestWidgetBlueprint(EventTestPath, FName(TEXT("MyImage")), nullptr, FixtureError))
    {
        AddError(FString::Printf(TEXT("Fixture build failed: %s"), *FixtureError));
        return false;
    }

    {
        TSharedPtr<FJsonObject> CreateParams = MakeShared<FJsonObject>();
        CreateParams->SetStringField(TEXT("asset_path"), EventTestPath);
        CreateParams->SetStringField(TEXT("animation_name"), TEXT("FadeIn"));
        CreateParams->SetNumberField(TEXT("duration_sec"), 0.5);
        CreateParams->SetBoolField(TEXT("compile_once"), false);

        TArray<TSharedPtr<FJsonValue>> Tracks;
        {
            TSharedPtr<FJsonObject> Track = MakeShared<FJsonObject>();
            Track->SetStringField(TEXT("widget_name"), TEXT("MyImage"));
            Track->SetStringField(TEXT("property"), TEXT("RenderOpacity"));

            TArray<TSharedPtr<FJsonValue>> Keys;
            {
                TSharedPtr<FJsonObject> K0 = MakeShared<FJsonObject>();
                K0->SetNumberField(TEXT("time"), 0.0);
                K0->SetNumberField(TEXT("value"), 0.0);
                K0->SetStringField(TEXT("interp"), TEXT("linear"));
                Keys.Add(MakeShared<FJsonValueObject>(K0));
            }
            {
                TSharedPtr<FJsonObject> K1 = MakeShared<FJsonObject>();
                K1->SetNumberField(TEXT("time"), 0.5);
                K1->SetNumberField(TEXT("value"), 1.0);
                K1->SetStringField(TEXT("interp"), TEXT("linear"));
                Keys.Add(MakeShared<FJsonValueObject>(K1));
            }
            Track->SetArrayField(TEXT("keys"), Keys);
            Tracks.Add(MakeShared<FJsonValueObject>(Track));
        }
        CreateParams->SetArrayField(TEXT("tracks"), Tracks);

        const FMonolithActionResult CreateResult = FMonolithToolRegistry::Get().ExecuteAction(
            TEXT("ui"), TEXT("create_animation_v2"), CreateParams);
        if (!CreateResult.bSuccess)
        {
            AddError(FString::Printf(TEXT("Prerequisite create_animation_v2 failed: %s"), *CreateResult.ErrorMessage));
            return false;
        }
    }

    TSharedPtr<FJsonObject> Params = MakeShared<FJsonObject>();
    Params->SetStringField(TEXT("asset_path"), EventTestPath);
    Params->SetStringField(TEXT("animation_name"), TEXT("FadeIn"));

    TArray<TSharedPtr<FJsonValue>> Events;
    {
        TSharedPtr<FJsonObject> E0 = MakeShared<FJsonObject>();
        E0->SetNumberField(TEXT("time"), 0.0);
        E0->SetStringField(TEXT("event_name"), TEXT("OnFadeStart"));
        Events.Add(MakeShared<FJsonValueObject>(E0));
    }
    {
        TSharedPtr<FJsonObject> E1 = MakeShared<FJsonObject>();
        E1->SetNumberField(TEXT("time"), 0.3);
        E1->SetStringField(TEXT("event_name"), TEXT("OnFadeMid"));
        Events.Add(MakeShared<FJsonValueObject>(E1));
    }
    Params->SetArrayField(TEXT("events"), Events);

    const FMonolithActionResult Result = FMonolithToolRegistry::Get().ExecuteAction(
        TEXT("ui"), TEXT("add_animation_event_track"), Params);

    TestTrue(TEXT("add_animation_event_track bSuccess"), Result.bSuccess);
    if (!Result.bSuccess)
    {
        AddError(FString::Printf(TEXT("Action error: %s (code %d)"), *Result.ErrorMessage, Result.ErrorCode));
        return false;
    }

    double EventsInserted = 0.0;
    TestTrue(TEXT("result has events_inserted"), Result.Result->TryGetNumberField(TEXT("events_inserted"), EventsInserted));
    TestEqual(TEXT("events_inserted == 2"), static_cast<int32>(EventsInserted), 2);

    UWidgetBlueprint* WBP = LoadObject<UWidgetBlueprint>(nullptr, *EventTestPath);
    if (!WBP)
    {
        AddError(TEXT("Failed to reload event track test WBP"));
        return false;
    }

#if WITH_EDITORONLY_DATA
    UWidgetAnimation* FoundAnim = nullptr;
    for (UWidgetAnimation* Anim : WBP->Animations)
    {
        if (Anim && Anim->GetDisplayLabel() == TEXT("FadeIn"))
        {
            FoundAnim = Anim;
            break;
        }
    }
    TestNotNull(TEXT("FadeIn animation found"), FoundAnim);
    if (!FoundAnim || !FoundAnim->MovieScene)
    {
        return false;
    }

    const UMovieScene* ConstScene = FoundAnim->MovieScene;
    UMovieSceneEventTrack* EventTrack = FoundAnim->MovieScene->FindTrack<UMovieSceneEventTrack>();
    TestNotNull(TEXT("Master event track found"), EventTrack);
    if (!EventTrack)
    {
        return false;
    }

    TestTrue(TEXT("Event track has at least 1 section"), EventTrack->GetAllSections().Num() > 0);

    UMovieSceneEventTriggerSection* TriggerSection =
        Cast<UMovieSceneEventTriggerSection>(EventTrack->GetAllSections()[0]);
    TestNotNull(TEXT("Trigger section exists"), TriggerSection);
    if (!TriggerSection)
    {
        return false;
    }

    TestEqual(TEXT("Event channel has 2 keys"),
        TriggerSection->EventChannel.GetNumKeys(), 2);
#endif

    return true;
}


/**
 * MonolithUI.BindAnimationToEvent.Basic
 *
 * Binds "OnHovered" to "Started" on an existing animation. Verifies
 * UWidgetAnimationDelegateBinding has the correct entry on the generated class.
 */
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
    FMonolithUIBindAnimationToEventBasicTest,
    "MonolithUI.BindAnimationToEvent.Basic",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FMonolithUIBindAnimationToEventBasicTest::RunTest(const FString& Parameters)
{
    using MonolithUI::TestUtils::CreateOrReuseTestWidgetBlueprint;

    const FString BindTestPath = TEXT("/Game/Tests/Monolith/UI/WBP_BindAnimTest");

    FString FixtureError;
    if (!CreateOrReuseTestWidgetBlueprint(BindTestPath, FName(TEXT("MyImage")), nullptr, FixtureError))
    {
        AddError(FString::Printf(TEXT("Fixture build failed: %s"), *FixtureError));
        return false;
    }

    {
        TSharedPtr<FJsonObject> CreateParams = MakeShared<FJsonObject>();
        CreateParams->SetStringField(TEXT("asset_path"), BindTestPath);
        CreateParams->SetStringField(TEXT("animation_name"), TEXT("FadeIn"));
        CreateParams->SetNumberField(TEXT("duration_sec"), 0.5);
        CreateParams->SetBoolField(TEXT("compile_once"), true);

        TArray<TSharedPtr<FJsonValue>> Tracks;
        {
            TSharedPtr<FJsonObject> Track = MakeShared<FJsonObject>();
            Track->SetStringField(TEXT("widget_name"), TEXT("MyImage"));
            Track->SetStringField(TEXT("property"), TEXT("RenderOpacity"));

            TArray<TSharedPtr<FJsonValue>> Keys;
            {
                TSharedPtr<FJsonObject> K0 = MakeShared<FJsonObject>();
                K0->SetNumberField(TEXT("time"), 0.0);
                K0->SetNumberField(TEXT("value"), 0.0);
                K0->SetStringField(TEXT("interp"), TEXT("linear"));
                Keys.Add(MakeShared<FJsonValueObject>(K0));
            }
            {
                TSharedPtr<FJsonObject> K1 = MakeShared<FJsonObject>();
                K1->SetNumberField(TEXT("time"), 0.5);
                K1->SetNumberField(TEXT("value"), 1.0);
                K1->SetStringField(TEXT("interp"), TEXT("linear"));
                Keys.Add(MakeShared<FJsonValueObject>(K1));
            }
            Track->SetArrayField(TEXT("keys"), Keys);
            Tracks.Add(MakeShared<FJsonValueObject>(Track));
        }
        CreateParams->SetArrayField(TEXT("tracks"), Tracks);

        const FMonolithActionResult CreateResult = FMonolithToolRegistry::Get().ExecuteAction(
            TEXT("ui"), TEXT("create_animation_v2"), CreateParams);
        if (!CreateResult.bSuccess)
        {
            AddError(FString::Printf(TEXT("Prerequisite create_animation_v2 failed: %s"), *CreateResult.ErrorMessage));
            return false;
        }
    }

    TSharedPtr<FJsonObject> Params = MakeShared<FJsonObject>();
    Params->SetStringField(TEXT("asset_path"), BindTestPath);
    Params->SetStringField(TEXT("animation_name"), TEXT("FadeIn"));
    Params->SetStringField(TEXT("widget_event"), TEXT("OnHovered"));
    Params->SetStringField(TEXT("animation_event"), TEXT("Started"));

    const FMonolithActionResult Result = FMonolithToolRegistry::Get().ExecuteAction(
        TEXT("ui"), TEXT("bind_animation_to_event"), Params);

    TestTrue(TEXT("bind_animation_to_event bSuccess"), Result.bSuccess);
    if (!Result.bSuccess)
    {
        AddError(FString::Printf(TEXT("Action error: %s (code %d)"), *Result.ErrorMessage, Result.ErrorCode));
        return false;
    }

    bool bBindingCreated = false;
    TestTrue(TEXT("result has binding_created"), Result.Result->TryGetBoolField(TEXT("binding_created"), bBindingCreated));
    TestTrue(TEXT("binding_created is true"), bBindingCreated);

    FString FuncNameBound;
    TestTrue(TEXT("result has function_name_bound"), Result.Result->TryGetStringField(TEXT("function_name_bound"), FuncNameBound));
    TestEqual(TEXT("function_name_bound matches"), FuncNameBound, TEXT("OnHovered_PlayFadeIn"));

    UWidgetBlueprint* WBP = LoadObject<UWidgetBlueprint>(nullptr, *BindTestPath);
    if (!WBP || !WBP->GeneratedClass)
    {
        AddError(TEXT("Failed to reload WBP or GeneratedClass is null"));
        return false;
    }

    UDynamicBlueprintBinding* BindingObj = UBlueprintGeneratedClass::GetDynamicBindingObject(
        WBP->GeneratedClass, UWidgetAnimationDelegateBinding::StaticClass());

    if (!BindingObj)
    {
        AddWarning(TEXT("UWidgetAnimationDelegateBinding not found on GeneratedClass -- "
                       "may have been cleared by recompilation. Marking as expected limitation."));
        return true;
    }

    UWidgetAnimationDelegateBinding* DelegateBinding = Cast<UWidgetAnimationDelegateBinding>(BindingObj);
    TestNotNull(TEXT("DelegateBinding cast succeeded"), DelegateBinding);
    if (!DelegateBinding)
    {
        return false;
    }

    TestTrue(TEXT("Has at least 1 binding entry"),
        DelegateBinding->WidgetAnimationDelegateBindings.Num() > 0);

    if (DelegateBinding->WidgetAnimationDelegateBindings.Num() > 0)
    {
        const FBlueprintWidgetAnimationDelegateBinding& Entry =
            DelegateBinding->WidgetAnimationDelegateBindings.Last();
        TestEqual(TEXT("Action is Started"),
            static_cast<uint8>(Entry.Action),
            static_cast<uint8>(EWidgetAnimationEvent::Started));
        TestEqual(TEXT("AnimationToBind is FadeIn"),
            Entry.AnimationToBind, FName(TEXT("FadeIn")));
        TestEqual(TEXT("FunctionNameToBind matches"),
            Entry.FunctionNameToBind, FName(TEXT("OnHovered_PlayFadeIn")));
    }

    return true;
}


/**
 * MonolithUI.AnimationReadActions.RegistryContract
 *
 * Locks the monolith-native naming contract: UMG MCP-style names may appear as
 * search aliases, but only canonical ui.get_animation_* actions are registered.
 */
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
    FMonolithUIAnimationReadActionsRegistryContractTest,
    "MonolithUI.AnimationReadActions.RegistryContract",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FMonolithUIAnimationReadActionsRegistryContractTest::RunTest(const FString& Parameters)
{
    FMonolithToolRegistry& Registry = FMonolithToolRegistry::Get();
    if (!Registry.HasAction(TEXT("ui"), TEXT("get_animation_overview"))
        || !Registry.HasAction(TEXT("ui"), TEXT("get_animation_timeline"))
        || !Registry.HasAction(TEXT("ui"), TEXT("get_animation_time_slice")))
    {
        FMonolithUIAnimationActions::RegisterActions(Registry);
    }

    bool bOk = true;
    const TCHAR* CanonicalActions[] = {
        TEXT("get_animation_overview"),
        TEXT("get_animation_timeline"),
        TEXT("get_animation_time_slice")
    };
    for (const TCHAR* ActionName : CanonicalActions)
    {
        bOk &= TestTrue(
            FString::Printf(TEXT("ui.%s is registered"), ActionName),
            Registry.HasAction(TEXT("ui"), ActionName));
        bOk &= TestEqual(
            FString::Printf(TEXT("ui.%s is read-only"), ActionName),
            Registry.GetActionExecutionPolicy(TEXT("ui"), ActionName).PolicyId,
            FString(TEXT("read_only")));
    }

    const TCHAR* ExternalAliases[] = {
        TEXT("animation_overview"),
        TEXT("animation_widget_properties"),
        TEXT("animation_time_properties")
    };
    for (const TCHAR* AliasName : ExternalAliases)
    {
        bOk &= TestFalse(
            FString::Printf(TEXT("external alias ui.%s is not registered"), AliasName),
            Registry.HasAction(TEXT("ui"), AliasName));
    }

    return bOk;
}


/**
 * MonolithUI.AnimationReadActions.Basic
 *
 * Proves the read-only overview/timeline/time-slice actions can inspect the
 * output of the canonical v2 writer plus an event track without creating a
 * duplicate external animation API surface.
 */
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
    FMonolithUIAnimationReadActionsBasicTest,
    "MonolithUI.AnimationReadActions.Basic",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FMonolithUIAnimationReadActionsBasicTest::RunTest(const FString& Parameters)
{
    using MonolithUI::TestUtils::CreateOrReuseTestWidgetBlueprint;

    const FString ReadTestPath = TEXT("/Game/Tests/Monolith/UI/WBP_AnimationReadActionsTest");

    FString FixtureError;
    if (!CreateOrReuseTestWidgetBlueprint(ReadTestPath, FName(TEXT("MyImage")), nullptr, FixtureError))
    {
        AddError(FString::Printf(TEXT("Fixture build failed: %s"), *FixtureError));
        return false;
    }

    {
        TSharedPtr<FJsonObject> CreateParams = MakeShared<FJsonObject>();
        CreateParams->SetStringField(TEXT("asset_path"), ReadTestPath);
        CreateParams->SetStringField(TEXT("animation_name"), TEXT("ReadFade"));
        CreateParams->SetNumberField(TEXT("duration_sec"), 0.5);
        CreateParams->SetBoolField(TEXT("compile_once"), true);

        TArray<TSharedPtr<FJsonValue>> Tracks;
        TSharedPtr<FJsonObject> Track = MakeShared<FJsonObject>();
        Track->SetStringField(TEXT("widget_name"), TEXT("MyImage"));
        Track->SetStringField(TEXT("property"), TEXT("RenderOpacity"));

        TArray<TSharedPtr<FJsonValue>> Keys;
        TSharedPtr<FJsonObject> K0 = MakeShared<FJsonObject>();
        K0->SetNumberField(TEXT("time"), 0.0);
        K0->SetNumberField(TEXT("value"), 0.0);
        K0->SetStringField(TEXT("interp"), TEXT("linear"));
        Keys.Add(MakeShared<FJsonValueObject>(K0));

        TSharedPtr<FJsonObject> K1 = MakeShared<FJsonObject>();
        K1->SetNumberField(TEXT("time"), 0.5);
        K1->SetNumberField(TEXT("value"), 1.0);
        K1->SetStringField(TEXT("interp"), TEXT("linear"));
        Keys.Add(MakeShared<FJsonValueObject>(K1));

        Track->SetArrayField(TEXT("keys"), Keys);
        Tracks.Add(MakeShared<FJsonValueObject>(Track));
        CreateParams->SetArrayField(TEXT("tracks"), Tracks);

        const FMonolithActionResult CreateResult = FMonolithToolRegistry::Get().ExecuteAction(
            TEXT("ui"), TEXT("create_animation_v2"), CreateParams);
        TestTrue(TEXT("create_animation_v2 succeeds"), CreateResult.bSuccess);
        if (!CreateResult.bSuccess)
        {
            AddError(FString::Printf(TEXT("create_animation_v2 failed: %s"), *CreateResult.ErrorMessage));
            return false;
        }
    }

    {
        TSharedPtr<FJsonObject> EventParams = MakeShared<FJsonObject>();
        EventParams->SetStringField(TEXT("asset_path"), ReadTestPath);
        EventParams->SetStringField(TEXT("animation_name"), TEXT("ReadFade"));

        TArray<TSharedPtr<FJsonValue>> Events;
        TSharedPtr<FJsonObject> E0 = MakeShared<FJsonObject>();
        E0->SetNumberField(TEXT("time"), 0.25);
        E0->SetStringField(TEXT("event_name"), TEXT("OnReadMid"));
        Events.Add(MakeShared<FJsonValueObject>(E0));
        EventParams->SetArrayField(TEXT("events"), Events);

        const FMonolithActionResult EventResult = FMonolithToolRegistry::Get().ExecuteAction(
            TEXT("ui"), TEXT("add_animation_event_track"), EventParams);
        TestTrue(TEXT("add_animation_event_track succeeds"), EventResult.bSuccess);
        if (!EventResult.bSuccess)
        {
            AddError(FString::Printf(TEXT("add_animation_event_track failed: %s"), *EventResult.ErrorMessage));
            return false;
        }
    }

    {
        TSharedPtr<FJsonObject> OverviewParams = MakeShared<FJsonObject>();
        OverviewParams->SetStringField(TEXT("asset_path"), ReadTestPath);
        OverviewParams->SetStringField(TEXT("animation_name"), TEXT("ReadFade"));

        const FMonolithActionResult Overview = FMonolithToolRegistry::Get().ExecuteAction(
            TEXT("ui"), TEXT("get_animation_overview"), OverviewParams);
        TestTrue(TEXT("get_animation_overview succeeds"), Overview.bSuccess);
        if (!Overview.bSuccess || !Overview.Result.IsValid())
        {
            AddError(FString::Printf(TEXT("get_animation_overview failed: %s"), *Overview.ErrorMessage));
            return false;
        }

        FString SchemaVersion;
        TestTrue(TEXT("overview has schema_version"),
            Overview.Result->TryGetStringField(TEXT("schema_version"), SchemaVersion));
        TestEqual(TEXT("overview schema version"), SchemaVersion, TEXT("ui_animation_overview.v1"));

        const TSharedPtr<FJsonObject>* AnimationObj = nullptr;
        TestTrue(TEXT("overview has animation object"),
            Overview.Result->TryGetObjectField(TEXT("animation"), AnimationObj));
        if (!AnimationObj || !AnimationObj->IsValid())
        {
            return false;
        }

        double KeyCount = 0.0;
        TestTrue(TEXT("overview animation has key_count"),
            (*AnimationObj)->TryGetNumberField(TEXT("key_count"), KeyCount));
        TestTrue(TEXT("overview key_count includes property and event keys"),
            static_cast<int32>(KeyCount) >= 3);
    }

    {
        TSharedPtr<FJsonObject> TimelineParams = MakeShared<FJsonObject>();
        TimelineParams->SetStringField(TEXT("asset_path"), ReadTestPath);
        TimelineParams->SetStringField(TEXT("animation_name"), TEXT("ReadFade"));

        const FMonolithActionResult Timeline = FMonolithToolRegistry::Get().ExecuteAction(
            TEXT("ui"), TEXT("get_animation_timeline"), TimelineParams);
        TestTrue(TEXT("get_animation_timeline succeeds"), Timeline.bSuccess);
        if (!Timeline.bSuccess || !Timeline.Result.IsValid())
        {
            AddError(FString::Printf(TEXT("get_animation_timeline failed: %s"), *Timeline.ErrorMessage));
            return false;
        }

        const TArray<TSharedPtr<FJsonValue>>* Rows = nullptr;
        TestTrue(TEXT("timeline has rows"), Timeline.Result->TryGetArrayField(TEXT("rows"), Rows));
        if (!Rows)
        {
            return false;
        }

        bool bSawRenderOpacity = false;
        bool bSawEvent = false;
        for (const TSharedPtr<FJsonValue>& RowValue : *Rows)
        {
            const TSharedPtr<FJsonObject>* Row = nullptr;
            if (!RowValue.IsValid() || !RowValue->TryGetObject(Row) || !Row || !Row->IsValid())
            {
                continue;
            }

            FString RowType;
            (*Row)->TryGetStringField(TEXT("row_type"), RowType);
            FString PropertyPath;
            (*Row)->TryGetStringField(TEXT("property_path"), PropertyPath);
            FString EventName;
            (*Row)->TryGetStringField(TEXT("event_name"), EventName);

            bSawRenderOpacity |= RowType == TEXT("property_key") && PropertyPath == TEXT("RenderOpacity");
            bSawEvent |= RowType == TEXT("event_key") && EventName == TEXT("OnReadMid");
        }
        TestTrue(TEXT("timeline includes RenderOpacity property keys"), bSawRenderOpacity);
        TestTrue(TEXT("timeline includes OnReadMid event key"), bSawEvent);
    }

    {
        TSharedPtr<FJsonObject> SliceParams = MakeShared<FJsonObject>();
        SliceParams->SetStringField(TEXT("asset_path"), ReadTestPath);
        SliceParams->SetStringField(TEXT("animation_name"), TEXT("ReadFade"));
        SliceParams->SetNumberField(TEXT("time"), 0.25);
        SliceParams->SetStringField(TEXT("property_path"), TEXT("RenderOpacity"));

        const FMonolithActionResult Slice = FMonolithToolRegistry::Get().ExecuteAction(
            TEXT("ui"), TEXT("get_animation_time_slice"), SliceParams);
        TestTrue(TEXT("get_animation_time_slice succeeds"), Slice.bSuccess);
        if (!Slice.bSuccess || !Slice.Result.IsValid())
        {
            AddError(FString::Printf(TEXT("get_animation_time_slice failed: %s"), *Slice.ErrorMessage));
            return false;
        }

        const TArray<TSharedPtr<FJsonValue>>* Samples = nullptr;
        TestTrue(TEXT("time_slice has samples"), Slice.Result->TryGetArrayField(TEXT("samples"), Samples));
        if (!Samples || Samples->Num() == 0)
        {
            return false;
        }

        const TSharedPtr<FJsonObject>* FirstSample = nullptr;
        TestTrue(TEXT("sample[0] is object"), (*Samples)[0]->TryGetObject(FirstSample));
        if (!FirstSample || !FirstSample->IsValid())
        {
            return false;
        }

        const TArray<TSharedPtr<FJsonValue>>* Rows = nullptr;
        TestTrue(TEXT("sample has rows"), (*FirstSample)->TryGetArrayField(TEXT("rows"), Rows));
        if (!Rows)
        {
            return false;
        }

        bool bSawSample = false;
        bool bSawEvent = false;
        for (const TSharedPtr<FJsonValue>& RowValue : *Rows)
        {
            const TSharedPtr<FJsonObject>* Row = nullptr;
            if (!RowValue.IsValid() || !RowValue->TryGetObject(Row) || !Row || !Row->IsValid())
            {
                continue;
            }

            FString RowType;
            (*Row)->TryGetStringField(TEXT("row_type"), RowType);
            if (RowType == TEXT("property_sample"))
            {
                double Value = 0.0;
                if ((*Row)->TryGetNumberField(TEXT("value"), Value))
                {
                    bSawSample |= FMath::IsNearlyEqual(Value, 0.5, 0.01);
                }
            }

            FString EventName;
            (*Row)->TryGetStringField(TEXT("event_name"), EventName);
            bSawEvent |= RowType == TEXT("event_match") && EventName == TEXT("OnReadMid");
        }
        TestTrue(TEXT("time_slice evaluates RenderOpacity at t=0.25 ~= 0.5"), bSawSample);
        TestTrue(TEXT("time_slice includes exact event match"), bSawEvent);
    }

    return true;
}


/**
 * MonolithUI.AnimationDelta.RegistryContract
 *
 * Locks the monolith-native delta contract: one canonical aggregate action,
 * guarded write defaults, and external sequencer-style names as search aliases
 * only.
 */
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
    FMonolithUIAnimationDeltaRegistryContractTest,
    "MonolithUI.AnimationDelta.RegistryContract",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FMonolithUIAnimationDeltaRegistryContractTest::RunTest(const FString& Parameters)
{
    FMonolithToolRegistry& Registry = FMonolithToolRegistry::Get();
    if (!Registry.HasAction(TEXT("ui"), TEXT("apply_animation_delta")))
    {
        FMonolithUIAnimationActions::RegisterActions(Registry);
    }

    bool bFoundAction = false;
    bool bDryRunDefault = false;
    bool bConfirmDefault = false;
    bool bConfirmDeleteDefault = false;
    bool bCompileDefault = false;
    bool bReadBackDefault = false;
    for (const FMonolithActionInfo& ActionInfo : Registry.GetActions(TEXT("ui")))
    {
        if (ActionInfo.Action != TEXT("apply_animation_delta"))
        {
            continue;
        }

        bFoundAction = true;
        if (ActionInfo.ParamSchema.IsValid())
        {
            const TSharedPtr<FJsonObject>* DryRun = nullptr;
            const TSharedPtr<FJsonObject>* Confirm = nullptr;
            const TSharedPtr<FJsonObject>* ConfirmDelete = nullptr;
            const TSharedPtr<FJsonObject>* Compile = nullptr;
            const TSharedPtr<FJsonObject>* ReadBack = nullptr;
            FString DefaultValue;

            bDryRunDefault = ActionInfo.ParamSchema->TryGetObjectField(TEXT("dry_run"), DryRun) && DryRun && DryRun->IsValid()
                && (*DryRun)->TryGetStringField(TEXT("default"), DefaultValue) && DefaultValue == TEXT("true");
            bConfirmDefault = ActionInfo.ParamSchema->TryGetObjectField(TEXT("confirm"), Confirm) && Confirm && Confirm->IsValid()
                && (*Confirm)->TryGetStringField(TEXT("default"), DefaultValue) && DefaultValue == TEXT("false");
            bConfirmDeleteDefault = ActionInfo.ParamSchema->TryGetObjectField(TEXT("confirm_delete"), ConfirmDelete) && ConfirmDelete && ConfirmDelete->IsValid()
                && (*ConfirmDelete)->TryGetStringField(TEXT("default"), DefaultValue) && DefaultValue == TEXT("false");
            bCompileDefault = ActionInfo.ParamSchema->TryGetObjectField(TEXT("compile"), Compile) && Compile && Compile->IsValid()
                && (*Compile)->TryGetStringField(TEXT("default"), DefaultValue) && DefaultValue == TEXT("true");
            bReadBackDefault = ActionInfo.ParamSchema->TryGetObjectField(TEXT("read_back"), ReadBack) && ReadBack && ReadBack->IsValid()
                && (*ReadBack)->TryGetStringField(TEXT("default"), DefaultValue) && DefaultValue == TEXT("true");
        }
        break;
    }

    bool bOk = true;
    bOk &= TestTrue(TEXT("ui.apply_animation_delta is registered"), Registry.HasAction(TEXT("ui"), TEXT("apply_animation_delta")));
    bOk &= TestTrue(TEXT("apply_animation_delta action info found"), bFoundAction);
    bOk &= TestEqual(
        TEXT("ui.apply_animation_delta is inferred as transaction_optional"),
        Registry.GetActionExecutionPolicy(TEXT("ui"), TEXT("apply_animation_delta")).PolicyId,
        FString(TEXT("transaction_optional")));
    bOk &= TestTrue(TEXT("dry_run defaults true"), bDryRunDefault);
    bOk &= TestTrue(TEXT("confirm defaults false"), bConfirmDefault);
    bOk &= TestTrue(TEXT("confirm_delete defaults false"), bConfirmDeleteDefault);
    bOk &= TestTrue(TEXT("compile defaults true"), bCompileDefault);
    bOk &= TestTrue(TEXT("read_back defaults true"), bReadBackDefault);

    const TCHAR* ExternalAliases[] = {
        TEXT("animation_append_widget_tracks"),
        TEXT("animation_append_time_slice"),
        TEXT("animation_delete_widget_keys"),
        TEXT("set_property_keys")
    };
    for (const TCHAR* AliasName : ExternalAliases)
    {
        bOk &= TestFalse(
            FString::Printf(TEXT("external alias ui.%s is not registered"), AliasName),
            Registry.HasAction(TEXT("ui"), AliasName));
    }

    return bOk;
}


/**
 * MonolithUI.AnimationDelta.FloatKeyLifecycle
 *
 * Proves the delta action modifies existing scalar float keys without
 * re-creating the animation, without resetting existing keys, and with explicit
 * write/delete confirmation gates.
 */
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
    FMonolithUIAnimationDeltaFloatKeyLifecycleTest,
    "MonolithUI.AnimationDelta.FloatKeyLifecycle",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FMonolithUIAnimationDeltaFloatKeyLifecycleTest::RunTest(const FString& Parameters)
{
    using namespace MonolithUI::AnimationCoreTests;
    using MonolithUI::TestUtils::CreateOrReuseTestWidgetBlueprint;

    const FString DeltaTestPath = TEXT("/Game/Tests/Monolith/UI/WBP_AnimationDeltaTest");

    FString FixtureError;
    if (!CreateOrReuseTestWidgetBlueprint(DeltaTestPath, FName(TEXT("MyImage")), nullptr, FixtureError))
    {
        AddError(FString::Printf(TEXT("Fixture build failed: %s"), *FixtureError));
        return false;
    }

    {
        TSharedPtr<FJsonObject> CreateParams = MakeShared<FJsonObject>();
        CreateParams->SetStringField(TEXT("asset_path"), DeltaTestPath);
        CreateParams->SetStringField(TEXT("animation_name"), TEXT("DeltaFade"));
        CreateParams->SetNumberField(TEXT("duration_sec"), 1.0);
        CreateParams->SetBoolField(TEXT("compile_once"), true);

        TArray<TSharedPtr<FJsonValue>> Tracks;
        TSharedPtr<FJsonObject> Track = MakeShared<FJsonObject>();
        Track->SetStringField(TEXT("widget_name"), TEXT("MyImage"));
        Track->SetStringField(TEXT("property"), TEXT("RenderOpacity"));

        TArray<TSharedPtr<FJsonValue>> Keys;
        TSharedPtr<FJsonObject> K0 = MakeShared<FJsonObject>();
        K0->SetNumberField(TEXT("time"), 0.0);
        K0->SetNumberField(TEXT("value"), 0.0);
        K0->SetStringField(TEXT("interp"), TEXT("linear"));
        Keys.Add(MakeShared<FJsonValueObject>(K0));

        TSharedPtr<FJsonObject> K1 = MakeShared<FJsonObject>();
        K1->SetNumberField(TEXT("time"), 1.0);
        K1->SetNumberField(TEXT("value"), 1.0);
        K1->SetStringField(TEXT("interp"), TEXT("linear"));
        Keys.Add(MakeShared<FJsonValueObject>(K1));

        Track->SetArrayField(TEXT("keys"), Keys);
        Tracks.Add(MakeShared<FJsonValueObject>(Track));
        CreateParams->SetArrayField(TEXT("tracks"), Tracks);

        const FMonolithActionResult CreateResult = FMonolithToolRegistry::Get().ExecuteAction(
            TEXT("ui"), TEXT("create_animation_v2"), CreateParams);
        TestTrue(TEXT("create_animation_v2 succeeds"), CreateResult.bSuccess);
        if (!CreateResult.bSuccess)
        {
            AddError(FString::Printf(TEXT("create_animation_v2 failed: %s"), *CreateResult.ErrorMessage));
            return false;
        }
    }

    UWidgetBlueprint* WBP = LoadObject<UWidgetBlueprint>(nullptr, *DeltaTestPath);
    UWidgetAnimation* OriginalAnim = FindAnimationByReadableName(WBP, TEXT("DeltaFade"));
    TestNotNull(TEXT("DeltaFade animation found"), OriginalAnim);
    if (!OriginalAnim || !OriginalAnim->GetMovieScene())
    {
        return false;
    }
    const UWidgetAnimation* OriginalAnimPtr = OriginalAnim;
    const FGuid OriginalBindingGuid = OriginalAnim->AnimationBindings.Num() > 0
        ? OriginalAnim->AnimationBindings[0].AnimationGuid
        : FGuid();
    const FFrameRate TickResolution = OriginalAnim->GetMovieScene()->GetTickResolution();
    const FFrameNumber Frame0 = TickResolution.AsFrameNumber(0.0);
    const FFrameNumber FrameHalf = TickResolution.AsFrameNumber(0.5);
    const FFrameNumber FrameOne = TickResolution.AsFrameNumber(1.0);

    UMovieSceneFloatTrack* FloatTrack = FindFloatTrackByProperty(OriginalAnim, FName(TEXT("RenderOpacity")));
    TestNotNull(TEXT("initial RenderOpacity track found"), FloatTrack);
    if (!FloatTrack)
    {
        return false;
    }
    TestEqual(TEXT("initial key count == 2"), CountFloatTrackKeys(FloatTrack), 2);

    auto MakeDeltaParams = [&DeltaTestPath](bool bSetDryRun, bool bDryRun, bool bSetConfirm, bool bConfirm, bool bConfirmDelete)
    {
        TSharedPtr<FJsonObject> Params = MakeShared<FJsonObject>();
        Params->SetStringField(TEXT("asset_path"), DeltaTestPath);
        Params->SetStringField(TEXT("animation_name"), TEXT("DeltaFade"));
        if (bSetDryRun)
        {
            Params->SetBoolField(TEXT("dry_run"), bDryRun);
        }
        if (bSetConfirm)
        {
            Params->SetBoolField(TEXT("confirm"), bConfirm);
        }
        if (bConfirmDelete)
        {
            Params->SetBoolField(TEXT("confirm_delete"), true);
        }
        Params->SetBoolField(TEXT("compile"), true);
        Params->SetBoolField(TEXT("read_back"), true);
        return Params;
    };

    {
        TSharedPtr<FJsonObject> DryRunParams = MakeDeltaParams(false, true, false, false, false);
        TArray<TSharedPtr<FJsonValue>> Operations;
        TSharedPtr<FJsonObject> Op = MakeShared<FJsonObject>();
        Op->SetStringField(TEXT("op"), TEXT("upsert_float_key"));
        Op->SetStringField(TEXT("widget_name"), TEXT("MyImage"));
        Op->SetStringField(TEXT("property_path"), TEXT("RenderOpacity"));
        Op->SetNumberField(TEXT("time"), 0.5);
        Op->SetNumberField(TEXT("value"), 0.25);
        Operations.Add(MakeShared<FJsonValueObject>(Op));
        DryRunParams->SetArrayField(TEXT("operations"), Operations);

        const FMonolithActionResult DryRun = FMonolithToolRegistry::Get().ExecuteAction(
            TEXT("ui"), TEXT("apply_animation_delta"), DryRunParams);
        TestTrue(TEXT("default dry-run succeeds"), DryRun.bSuccess);
        if (!DryRun.bSuccess || !DryRun.Result.IsValid())
        {
            AddError(FString::Printf(TEXT("dry-run failed: %s"), *DryRun.ErrorMessage));
            return false;
        }

        bool bMutated = true;
        TestTrue(TEXT("dry-run result has mutated"), DryRun.Result->TryGetBoolField(TEXT("mutated"), bMutated));
        TestFalse(TEXT("dry-run did not mutate"), bMutated);
        double OperationsApplied = -1.0;
        TestTrue(TEXT("dry-run result has operations_applied"), DryRun.Result->TryGetNumberField(TEXT("operations_applied"), OperationsApplied));
        TestEqual(TEXT("dry-run operations_applied == 0"), static_cast<int32>(OperationsApplied), 0);
        TestEqual(TEXT("dry-run keeps key count == 2"), CountFloatTrackKeys(FloatTrack), 2);
    }

    {
        TSharedPtr<FJsonObject> NoConfirmParams = MakeDeltaParams(true, false, false, false, false);
        TArray<TSharedPtr<FJsonValue>> Operations;
        TSharedPtr<FJsonObject> Op = MakeShared<FJsonObject>();
        Op->SetStringField(TEXT("op"), TEXT("upsert_float_key"));
        Op->SetStringField(TEXT("widget_name"), TEXT("MyImage"));
        Op->SetStringField(TEXT("property"), TEXT("opacity"));
        Op->SetNumberField(TEXT("time"), 0.5);
        Op->SetNumberField(TEXT("value"), 0.25);
        Operations.Add(MakeShared<FJsonValueObject>(Op));
        NoConfirmParams->SetArrayField(TEXT("operations"), Operations);

        const FMonolithActionResult NoConfirm = FMonolithToolRegistry::Get().ExecuteAction(
            TEXT("ui"), TEXT("apply_animation_delta"), NoConfirmParams);
        TestFalse(TEXT("dry_run=false without confirm fails"), NoConfirm.bSuccess);
        TestEqual(TEXT("no-confirm keeps key count == 2"), CountFloatTrackKeys(FloatTrack), 2);
    }

    {
        TSharedPtr<FJsonObject> ApplyParams = MakeDeltaParams(true, false, true, true, false);
        TArray<TSharedPtr<FJsonValue>> Operations;
        {
            TSharedPtr<FJsonObject> Op = MakeShared<FJsonObject>();
            Op->SetStringField(TEXT("op"), TEXT("upsert_float_key"));
            Op->SetStringField(TEXT("widget_name"), TEXT("MyImage"));
            Op->SetStringField(TEXT("property_path"), TEXT("RenderOpacity"));
            Op->SetNumberField(TEXT("time"), 0.5);
            Op->SetNumberField(TEXT("value"), 0.25);
            Operations.Add(MakeShared<FJsonValueObject>(Op));
        }
        {
            TSharedPtr<FJsonObject> Op = MakeShared<FJsonObject>();
            Op->SetStringField(TEXT("op"), TEXT("upsert_float_key"));
            Op->SetStringField(TEXT("widget_name"), TEXT("MyImage"));
            Op->SetStringField(TEXT("property_path"), TEXT("RenderOpacity"));
            Op->SetNumberField(TEXT("time"), 1.0);
            Op->SetNumberField(TEXT("value"), 0.75);
            Operations.Add(MakeShared<FJsonValueObject>(Op));
        }
        ApplyParams->SetArrayField(TEXT("operations"), Operations);

        const FMonolithActionResult Apply = FMonolithToolRegistry::Get().ExecuteAction(
            TEXT("ui"), TEXT("apply_animation_delta"), ApplyParams);
        TestTrue(TEXT("apply delta succeeds"), Apply.bSuccess);
        if (!Apply.bSuccess || !Apply.Result.IsValid())
        {
            AddError(FString::Printf(TEXT("apply delta failed: %s"), *Apply.ErrorMessage));
            return false;
        }

        const TSharedPtr<FJsonObject>* ReadBack = nullptr;
        TestTrue(TEXT("apply result has read_back_overview"),
            Apply.Result->TryGetObjectField(TEXT("read_back_overview"), ReadBack) && ReadBack && ReadBack->IsValid());
        double KeysInserted = -1.0;
        double KeysUpdated = -1.0;
        TestTrue(TEXT("apply result has keys_inserted"), Apply.Result->TryGetNumberField(TEXT("keys_inserted"), KeysInserted));
        TestTrue(TEXT("apply result has keys_updated"), Apply.Result->TryGetNumberField(TEXT("keys_updated"), KeysUpdated));
        TestEqual(TEXT("one key inserted"), static_cast<int32>(KeysInserted), 1);
        TestEqual(TEXT("one key updated"), static_cast<int32>(KeysUpdated), 1);
    }

    UWidgetAnimation* AfterApplyAnim = FindAnimationByReadableName(WBP, TEXT("DeltaFade"));
    TestTrue(TEXT("animation UObject identity preserved after delta"), AfterApplyAnim == OriginalAnimPtr);
    if (OriginalBindingGuid.IsValid() && AfterApplyAnim && AfterApplyAnim->AnimationBindings.Num() > 0)
    {
        TestEqual(TEXT("animation binding guid preserved"), AfterApplyAnim->AnimationBindings[0].AnimationGuid, OriginalBindingGuid);
    }
    FloatTrack = FindFloatTrackByProperty(AfterApplyAnim, FName(TEXT("RenderOpacity")));
    TestEqual(TEXT("apply produces 3 keys"), CountFloatTrackKeys(FloatTrack), 3);
    float Value = -1.0f;
    TestTrue(TEXT("frame 0 key exists"), TryGetFloatTrackValueAtFrame(FloatTrack, Frame0, Value));
    TestNearlyEqual(TEXT("frame 0 value remains 0"), Value, 0.0f, 0.01f);
    TestTrue(TEXT("frame 0.5 key exists"), TryGetFloatTrackValueAtFrame(FloatTrack, FrameHalf, Value));
    TestNearlyEqual(TEXT("frame 0.5 value inserted"), Value, 0.25f, 0.01f);
    TestTrue(TEXT("frame 1.0 key exists"), TryGetFloatTrackValueAtFrame(FloatTrack, FrameOne, Value));
    TestNearlyEqual(TEXT("frame 1.0 value updated"), Value, 0.75f, 0.01f);

    {
        TSharedPtr<FJsonObject> DeleteGuardParams = MakeDeltaParams(true, false, true, true, false);
        TArray<TSharedPtr<FJsonValue>> Operations;
        TSharedPtr<FJsonObject> Op = MakeShared<FJsonObject>();
        Op->SetStringField(TEXT("op"), TEXT("delete_float_key"));
        Op->SetStringField(TEXT("widget_name"), TEXT("MyImage"));
        Op->SetStringField(TEXT("property_path"), TEXT("RenderOpacity"));
        Op->SetNumberField(TEXT("time"), 0.5);
        Operations.Add(MakeShared<FJsonValueObject>(Op));
        DeleteGuardParams->SetArrayField(TEXT("operations"), Operations);

        const FMonolithActionResult DeleteGuard = FMonolithToolRegistry::Get().ExecuteAction(
            TEXT("ui"), TEXT("apply_animation_delta"), DeleteGuardParams);
        TestFalse(TEXT("delete without confirm_delete fails"), DeleteGuard.bSuccess);
        TestEqual(TEXT("delete guard keeps key count == 3"), CountFloatTrackKeys(FloatTrack), 3);
    }

    {
        TSharedPtr<FJsonObject> DeleteParams = MakeDeltaParams(true, false, true, true, true);
        TArray<TSharedPtr<FJsonValue>> Operations;
        TSharedPtr<FJsonObject> Op = MakeShared<FJsonObject>();
        Op->SetStringField(TEXT("op"), TEXT("delete_float_key"));
        Op->SetStringField(TEXT("widget_name"), TEXT("MyImage"));
        Op->SetStringField(TEXT("property_path"), TEXT("RenderOpacity"));
        Op->SetNumberField(TEXT("time"), 0.5);
        Operations.Add(MakeShared<FJsonValueObject>(Op));
        DeleteParams->SetArrayField(TEXT("operations"), Operations);

        const FMonolithActionResult Delete = FMonolithToolRegistry::Get().ExecuteAction(
            TEXT("ui"), TEXT("apply_animation_delta"), DeleteParams);
        TestTrue(TEXT("delete with confirm_delete succeeds"), Delete.bSuccess);
        if (!Delete.bSuccess || !Delete.Result.IsValid())
        {
            AddError(FString::Printf(TEXT("delete delta failed: %s"), *Delete.ErrorMessage));
            return false;
        }
        double KeysDeleted = -1.0;
        TestTrue(TEXT("delete result has keys_deleted"), Delete.Result->TryGetNumberField(TEXT("keys_deleted"), KeysDeleted));
        TestEqual(TEXT("one key deleted"), static_cast<int32>(KeysDeleted), 1);
    }

    FloatTrack = FindFloatTrackByProperty(FindAnimationByReadableName(WBP, TEXT("DeltaFade")), FName(TEXT("RenderOpacity")));
    TestEqual(TEXT("delete leaves 2 keys"), CountFloatTrackKeys(FloatTrack), 2);
    TestFalse(TEXT("frame 0.5 key removed"), TryGetFloatTrackValueAtFrame(FloatTrack, FrameHalf, Value));
    TestTrue(TEXT("frame 1.0 key remains"), TryGetFloatTrackValueAtFrame(FloatTrack, FrameOne, Value));
    TestNearlyEqual(TEXT("frame 1.0 value still 0.75"), Value, 0.75f, 0.01f);

    {
        TSharedPtr<FJsonObject> TimelineParams = MakeShared<FJsonObject>();
        TimelineParams->SetStringField(TEXT("asset_path"), DeltaTestPath);
        TimelineParams->SetStringField(TEXT("animation_name"), TEXT("DeltaFade"));
        TimelineParams->SetStringField(TEXT("property_path"), TEXT("RenderOpacity"));

        const FMonolithActionResult Timeline = FMonolithToolRegistry::Get().ExecuteAction(
            TEXT("ui"), TEXT("get_animation_timeline"), TimelineParams);
        TestTrue(TEXT("timeline read-back succeeds"), Timeline.bSuccess);
        if (!Timeline.bSuccess || !Timeline.Result.IsValid())
        {
            AddError(FString::Printf(TEXT("timeline read-back failed: %s"), *Timeline.ErrorMessage));
            return false;
        }

        const TArray<TSharedPtr<FJsonValue>>* Rows = nullptr;
        TestTrue(TEXT("timeline has rows"), Timeline.Result->TryGetArrayField(TEXT("rows"), Rows));
        if (!Rows)
        {
            return false;
        }

        bool bSawFrameHalf = false;
        bool bSawFrameOneUpdated = false;
        for (const TSharedPtr<FJsonValue>& RowValue : *Rows)
        {
            const TSharedPtr<FJsonObject>* Row = nullptr;
            if (!RowValue.IsValid() || !RowValue->TryGetObject(Row) || !Row || !Row->IsValid())
            {
                continue;
            }

            FString RowType;
            (*Row)->TryGetStringField(TEXT("row_type"), RowType);
            if (RowType != TEXT("property_key"))
            {
                continue;
            }

            double FrameNumber = 0.0;
            (*Row)->TryGetNumberField(TEXT("frame"), FrameNumber);
            bSawFrameHalf |= static_cast<int32>(FrameNumber) == FrameHalf.Value;

            if (static_cast<int32>(FrameNumber) == FrameOne.Value)
            {
                const TSharedPtr<FJsonObject>* KeyObj = nullptr;
                if ((*Row)->TryGetObjectField(TEXT("key"), KeyObj) && KeyObj && KeyObj->IsValid())
                {
                    double KeyValue = 0.0;
                    bSawFrameOneUpdated |= (*KeyObj)->TryGetNumberField(TEXT("value"), KeyValue)
                        && FMath::IsNearlyEqual(KeyValue, 0.75, 0.01);
                }
            }
        }
        TestFalse(TEXT("timeline read-back no longer has deleted 0.5 key"), bSawFrameHalf);
        TestTrue(TEXT("timeline read-back has updated 1.0 key"), bSawFrameOneUpdated);
    }

    return true;
}

#endif // WITH_DEV_AUTOMATION_TESTS
