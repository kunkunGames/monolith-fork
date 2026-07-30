// MonolithUIAnimationActions.cpp
#include "MonolithUIAnimationActions.h"
#include "MonolithUIInternal.h"
#include "MonolithParamSchema.h"
#include "Animation/WidgetAnimation.h"
#include "MovieScene.h"
#include "MovieScenePossessable.h"
#include "MovieSceneSpawnable.h"
#include "Tracks/MovieSceneFloatTrack.h"
#include "Channels/MovieSceneFloatChannel.h"
#include "Tracks/MovieScene3DTransformTrack.h"
#include "Tracks/MovieSceneEventTrack.h"
#include "Sections/MovieSceneFloatSection.h"
#include "Sections/MovieScene3DTransformSection.h"
#include "Sections/MovieSceneEventTriggerSection.h"
#include "Channels/MovieSceneChannelProxy.h"
#include "Kismet2/BlueprintEditorUtils.h"
#include "Kismet2/KismetEditorUtilities.h"
#include "Animation/WidgetAnimationDelegateBinding.h"
#include "Channels/MovieSceneEventChannel.h"
#include "Channels/MovieSceneEvent.h"
#include "Engine/BlueprintGeneratedClass.h"
#include "Misc/FrameRate.h"
#include "Misc/FrameTime.h"

namespace
{
    FString ResolveBindingName(UMovieScene* MovieScene, const FGuid& BindingGuid)
    {
        if (!MovieScene)
        {
            return FString();
        }

        if (const FMovieScenePossessable* Possessable = MovieScene->FindPossessable(BindingGuid))
        {
            return Possessable->GetName();
        }
        if (const FMovieSceneSpawnable* Spawnable = MovieScene->FindSpawnable(BindingGuid))
        {
            return Spawnable->GetName();
        }
        return FString();
    }

    FString ResolveBindingName(UMovieScene* MovieScene, const FMovieSceneBinding& Binding)
    {
        const FGuid BindingGuid = Binding.GetObjectGuid();
        const FString BindingName = ResolveBindingName(MovieScene, BindingGuid);
        return BindingName.IsEmpty()
            ? BindingGuid.ToString(EGuidFormats::DigitsWithHyphensLower)
            : BindingName;
    }

    bool TryExtractKeyframeTime(const TSharedPtr<FJsonObject>& KfObj, double& OutTime, FMonolithActionResult& OutError)
    {
        if (!KfObj->TryGetNumberField(TEXT("time"), OutTime))
        {
            OutError = FMonolithActionResult::Error(TEXT("keyframe.time must be a number"), -32602);
            return false;
        }
        return true;
    }

    FGuid FindOrCreateWidgetAnimationBinding(UWidgetBlueprint* WBP, UWidgetAnimation* Animation, UMovieScene* MovieScene, UWidget* TargetWidget)
    {
        if (!WBP || !Animation || !MovieScene || !TargetWidget)
        {
            return FGuid();
        }

        const FName WidgetFName = TargetWidget->GetFName();

        for (const FWidgetAnimationBinding& Binding : Animation->AnimationBindings)
        {
            if (Binding.WidgetName == WidgetFName)
            {
                return Binding.AnimationGuid;
            }
        }

        const FString WidgetName = WidgetFName.ToString();
        const UMovieScene* ConstMovieScene = MovieScene;
        for (const FMovieSceneBinding& Binding : ConstMovieScene->GetBindings())
        {
            if (ResolveBindingName(MovieScene, Binding.GetObjectGuid()) == WidgetName)
            {
                FWidgetAnimationBinding AnimationBinding;
                AnimationBinding.AnimationGuid = Binding.GetObjectGuid();
                AnimationBinding.WidgetName = WidgetFName;
                AnimationBinding.SlotWidgetName = NAME_None;
                AnimationBinding.bIsRootWidget = (WBP->WidgetTree && WBP->WidgetTree->RootWidget == TargetWidget);
                Animation->AnimationBindings.Add(AnimationBinding);
                return AnimationBinding.AnimationGuid;
            }
        }

        const FGuid NewGuid = MovieScene->AddPossessable(WidgetFName.ToString(), TargetWidget->GetClass());

        FWidgetAnimationBinding Binding;
        Binding.AnimationGuid = NewGuid;
        Binding.WidgetName = WidgetFName;
        Binding.SlotWidgetName = NAME_None;
        Binding.bIsRootWidget = (WBP->WidgetTree && WBP->WidgetTree->RootWidget == TargetWidget);
        Animation->AnimationBindings.Add(Binding);
        return NewGuid;
    }

    UMovieSceneFloatTrack* FindOrCreateFloatTrack(
        UMovieScene* MovieScene,
        const FGuid& PossessableGuid,
        const FName& PropertyName,
        const FString& PropertyPath,
        const FFrameNumber& StartFrame,
        const FFrameNumber& EndFrame)
    {
        if (!MovieScene || !PossessableGuid.IsValid())
        {
            return nullptr;
        }

        const FMovieSceneBinding* Binding = static_cast<const UMovieScene*>(MovieScene)->FindBinding(PossessableGuid);
        if (Binding)
        {
            for (UMovieSceneTrack* Track : Binding->GetTracks())
            {
                UMovieSceneFloatTrack* FloatTrack = Cast<UMovieSceneFloatTrack>(Track);
                if (FloatTrack && FloatTrack->GetPropertyPath().ToString() == PropertyPath)
                {
                    return FloatTrack;
                }
            }
        }

        UMovieSceneFloatTrack* FloatTrack = MovieScene->AddTrack<UMovieSceneFloatTrack>(PossessableGuid);
        if (!FloatTrack)
        {
            return nullptr;
        }

        FloatTrack->SetPropertyNameAndPath(PropertyName, *PropertyPath);

        UMovieSceneFloatSection* Section = Cast<UMovieSceneFloatSection>(FloatTrack->CreateNewSection());
        if (!Section)
        {
            return nullptr;
        }

        Section->SetRange(TRange<FFrameNumber>(StartFrame, EndFrame));
        FloatTrack->AddSection(*Section);
        return FloatTrack;
    }

    UMovieSceneFloatTrack* FindOrCreateOpacityTrack(
        UMovieScene* MovieScene,
        const FGuid& PossessableGuid,
        const FFrameNumber& StartFrame,
        const FFrameNumber& EndFrame)
    {
        return FindOrCreateFloatTrack(
            MovieScene,
            PossessableGuid,
            FName(TEXT("RenderOpacity")),
            TEXT("RenderOpacity"),
            StartFrame,
            EndFrame);
    }

    FString GetAnimationReadableName(const UWidgetAnimation* Animation)
    {
        if (!Animation)
        {
            return FString();
        }
#if WITH_EDITORONLY_DATA
        const FString DisplayLabel = Animation->GetDisplayLabel();
        if (!DisplayLabel.IsEmpty())
        {
            return DisplayLabel;
        }
#endif
        return Animation->GetName();
    }

    UWidgetAnimation* FindAnimationForRead(UWidgetBlueprint* WBP, const FString& AnimationName)
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
            if (Animation->GetName() == AnimationName || GetAnimationReadableName(Animation) == AnimationName)
            {
                return Animation;
            }
        }
        return nullptr;
    }

    double FrameToSeconds(const FFrameNumber& Frame, const FFrameRate& TickResolution)
    {
        const double Rate = TickResolution.AsDecimal();
        return Rate > 0.0 ? static_cast<double>(Frame.Value) / Rate : 0.0;
    }

    FString GuidToString(const FGuid& Guid)
    {
        return Guid.IsValid() ? Guid.ToString(EGuidFormats::DigitsWithHyphensLower) : FString();
    }

    FString InterpModeToString(const ERichCurveInterpMode Mode)
    {
        switch (Mode)
        {
        case RCIM_Linear: return TEXT("linear");
        case RCIM_Constant: return TEXT("constant");
        case RCIM_Cubic: return TEXT("cubic");
        case RCIM_None: return TEXT("none");
        default: return TEXT("unknown");
        }
    }

    FString TangentModeToString(const ERichCurveTangentMode Mode)
    {
        switch (Mode)
        {
        case RCTM_Auto: return TEXT("auto");
        case RCTM_User: return TEXT("user");
        case RCTM_Break: return TEXT("break");
        case RCTM_None: return TEXT("none");
        default: return TEXT("unknown");
        }
    }

    FString TangentWeightModeToString(const ERichCurveTangentWeightMode Mode)
    {
        switch (Mode)
        {
        case RCTWM_WeightedNone: return TEXT("none");
        case RCTWM_WeightedArrive: return TEXT("arrive");
        case RCTWM_WeightedLeave: return TEXT("leave");
        case RCTWM_WeightedBoth: return TEXT("both");
        default: return TEXT("unknown");
        }
    }

    FString WidgetAnimationEventToString(const EWidgetAnimationEvent Action)
    {
        switch (Action)
        {
        case EWidgetAnimationEvent::Started: return TEXT("Started");
        case EWidgetAnimationEvent::Finished: return TEXT("Finished");
        default: return TEXT("Unknown");
        }
    }

    bool IsFrameInSection(const UMovieSceneSection* Section, const FFrameNumber& Frame)
    {
        if (!Section)
        {
            return false;
        }
        const TRange<FFrameNumber> Range = Section->GetRange();
        return !Range.HasLowerBound() || !Range.HasUpperBound() || Range.Contains(Frame);
    }

    TSharedPtr<FJsonObject> MakeTimeObject(const FFrameNumber& Frame, const FFrameRate& TickResolution)
    {
        TSharedPtr<FJsonObject> Obj = MakeShared<FJsonObject>();
        Obj->SetNumberField(TEXT("frame"), Frame.Value);
        Obj->SetNumberField(TEXT("time"), FrameToSeconds(Frame, TickResolution));
        return Obj;
    }

    void AddSectionRange(TSharedPtr<FJsonObject>& Obj, const UMovieSceneSection* Section, const FFrameRate& TickResolution)
    {
        if (!Obj.IsValid() || !Section)
        {
            return;
        }
        const TRange<FFrameNumber> Range = Section->GetRange();
        if (Range.HasLowerBound())
        {
            Obj->SetNumberField(TEXT("section_start_frame"), Range.GetLowerBoundValue().Value);
            Obj->SetNumberField(TEXT("section_start_time"), FrameToSeconds(Range.GetLowerBoundValue(), TickResolution));
        }
        if (Range.HasUpperBound())
        {
            Obj->SetNumberField(TEXT("section_end_frame"), Range.GetUpperBoundValue().Value);
            Obj->SetNumberField(TEXT("section_end_time"), FrameToSeconds(Range.GetUpperBoundValue(), TickResolution));
        }
    }

    TSharedPtr<FJsonObject> MakeValueObject(const FMovieSceneFloatValue& Value)
    {
        TSharedPtr<FJsonObject> Obj = MakeShared<FJsonObject>();
        Obj->SetNumberField(TEXT("value"), Value.Value);
        Obj->SetStringField(TEXT("interp"), InterpModeToString(Value.InterpMode));
        Obj->SetStringField(TEXT("tangent_mode"), TangentModeToString(Value.TangentMode));
        Obj->SetNumberField(TEXT("arrive_tangent"), Value.Tangent.ArriveTangent);
        Obj->SetNumberField(TEXT("leave_tangent"), Value.Tangent.LeaveTangent);
        Obj->SetNumberField(TEXT("arrive_weight"), Value.Tangent.ArriveTangentWeight);
        Obj->SetNumberField(TEXT("leave_weight"), Value.Tangent.LeaveTangentWeight);
        Obj->SetStringField(TEXT("tangent_weight_mode"), TangentWeightModeToString(Value.Tangent.TangentWeightMode));
        return Obj;
    }

    const FWidgetAnimationBinding* FindWidgetAnimationBinding(const UWidgetAnimation* Animation, const FGuid& BindingGuid)
    {
        if (!Animation || !BindingGuid.IsValid())
        {
            return nullptr;
        }
        for (const FWidgetAnimationBinding& Binding : Animation->AnimationBindings)
        {
            if (Binding.AnimationGuid == BindingGuid)
            {
                return &Binding;
            }
        }
        return nullptr;
    }

    FString ResolveWidgetNameForBinding(const UWidgetAnimation* Animation, UMovieScene* MovieScene, const FGuid& BindingGuid)
    {
        if (const FWidgetAnimationBinding* Binding = FindWidgetAnimationBinding(Animation, BindingGuid))
        {
            return Binding->WidgetName.ToString();
        }
        return ResolveBindingName(MovieScene, BindingGuid);
    }

    TSharedPtr<FJsonObject> MakeBindingIdentity(
        const UWidgetAnimation* Animation,
        UMovieScene* MovieScene,
        const FGuid& BindingGuid)
    {
        TSharedPtr<FJsonObject> Obj = MakeShared<FJsonObject>();
        Obj->SetStringField(TEXT("binding_guid"), GuidToString(BindingGuid));
        Obj->SetStringField(TEXT("binding_name"), ResolveBindingName(MovieScene, BindingGuid));
        Obj->SetStringField(TEXT("widget_name"), ResolveWidgetNameForBinding(Animation, MovieScene, BindingGuid));

        if (const FWidgetAnimationBinding* Binding = FindWidgetAnimationBinding(Animation, BindingGuid))
        {
            Obj->SetStringField(TEXT("slot_widget_name"), Binding->SlotWidgetName.ToString());
            Obj->SetBoolField(TEXT("is_root_widget"), Binding->bIsRootWidget);
        }
        else
        {
            Obj->SetBoolField(TEXT("has_widget_animation_binding"), false);
        }
        return Obj;
    }

    int32 CountFloatKeys(const UMovieSceneFloatTrack* FloatTrack)
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

    int32 CountEventKeys(const UMovieSceneEventTrack* EventTrack)
    {
        int32 Count = 0;
        if (!EventTrack)
        {
            return Count;
        }
        for (UMovieSceneSection* Section : EventTrack->GetAllSections())
        {
            if (const UMovieSceneEventTriggerSection* TriggerSection = Cast<UMovieSceneEventTriggerSection>(Section))
            {
                Count += TriggerSection->EventChannel.GetNumKeys();
            }
        }
        return Count;
    }

    TSharedPtr<FJsonObject> MakeFloatTrackSummary(
        const UWidgetAnimation* Animation,
        UMovieScene* MovieScene,
        const FMovieSceneBinding& Binding,
        const UMovieSceneFloatTrack* FloatTrack,
        const int32 TrackIndex)
    {
        TSharedPtr<FJsonObject> Obj = MakeShared<FJsonObject>();
        Obj->SetStringField(TEXT("track_kind"), TEXT("property_float"));
        Obj->SetStringField(TEXT("track_class"), FloatTrack ? FloatTrack->GetClass()->GetName() : FString());
        Obj->SetNumberField(TEXT("track_index"), TrackIndex);
        Obj->SetStringField(TEXT("track_id"), FString::Printf(
            TEXT("%s:%s:%d"),
            *GuidToString(Binding.GetObjectGuid()),
            FloatTrack ? *FloatTrack->GetPropertyPath().ToString() : TEXT(""),
            TrackIndex));
        Obj->SetObjectField(TEXT("binding"), MakeBindingIdentity(Animation, MovieScene, Binding.GetObjectGuid()));
        if (FloatTrack)
        {
            Obj->SetStringField(TEXT("property_name"), FloatTrack->GetPropertyName().ToString());
            Obj->SetStringField(TEXT("property_path"), FloatTrack->GetPropertyPath().ToString());
            Obj->SetStringField(TEXT("display_name"), FloatTrack->GetDisplayName().ToString());
            Obj->SetNumberField(TEXT("section_count"), FloatTrack->GetAllSections().Num());
            Obj->SetNumberField(TEXT("key_count"), CountFloatKeys(FloatTrack));
        }
        Obj->SetBoolField(TEXT("timeline_supported"), true);
        Obj->SetBoolField(TEXT("time_slice_supported"), true);
        return Obj;
    }

    TSharedPtr<FJsonObject> MakeEventTrackSummary(const UMovieSceneEventTrack* EventTrack, const int32 TrackIndex)
    {
        TSharedPtr<FJsonObject> Obj = MakeShared<FJsonObject>();
        Obj->SetStringField(TEXT("track_kind"), TEXT("event"));
        Obj->SetStringField(TEXT("track_class"), EventTrack ? EventTrack->GetClass()->GetName() : FString());
        Obj->SetNumberField(TEXT("track_index"), TrackIndex);
        Obj->SetStringField(TEXT("track_id"), FString::Printf(TEXT("master_event:%d"), TrackIndex));
        if (EventTrack)
        {
            Obj->SetStringField(TEXT("display_name"), EventTrack->GetDisplayName().ToString());
            Obj->SetNumberField(TEXT("section_count"), EventTrack->GetAllSections().Num());
            Obj->SetNumberField(TEXT("key_count"), CountEventKeys(EventTrack));
        }
        Obj->SetBoolField(TEXT("timeline_supported"), true);
        Obj->SetBoolField(TEXT("time_slice_supported"), false);
        Obj->SetStringField(TEXT("time_slice_mode"), TEXT("exact_frame_match_only"));
        return Obj;
    }

    void AppendDelegateBindingRows(UWidgetBlueprint* WBP, const FString& AnimationName, TArray<TSharedPtr<FJsonValue>>& OutRows)
    {
        if (!WBP || !WBP->GeneratedClass)
        {
            return;
        }

        UDynamicBlueprintBinding* BindingObj = UBlueprintGeneratedClass::GetDynamicBindingObject(
            WBP->GeneratedClass, UWidgetAnimationDelegateBinding::StaticClass());
        const UWidgetAnimationDelegateBinding* DelegateBinding = Cast<UWidgetAnimationDelegateBinding>(BindingObj);
        if (!DelegateBinding)
        {
            return;
        }

        OutRows.Reserve(DelegateBinding->WidgetAnimationDelegateBindings.Num());
        for (const FBlueprintWidgetAnimationDelegateBinding& Row : DelegateBinding->WidgetAnimationDelegateBindings)
        {
            if (!AnimationName.IsEmpty() && Row.AnimationToBind.ToString() != AnimationName)
            {
                continue;
            }
            TSharedPtr<FJsonObject> Obj = MakeShared<FJsonObject>();
            Obj->SetStringField(TEXT("animation_name"), Row.AnimationToBind.ToString());
            Obj->SetStringField(TEXT("action"), WidgetAnimationEventToString(Row.Action));
            Obj->SetStringField(TEXT("function_name"), Row.FunctionNameToBind.ToString());
            Obj->SetStringField(TEXT("user_tag"), Row.UserTag.ToString());
            OutRows.Add(MakeShared<FJsonValueObject>(Obj));
        }
    }

    void AppendFloatTimelineRows(
        const UWidgetAnimation* Animation,
        UMovieScene* MovieScene,
        const FMovieSceneBinding& Binding,
        const UMovieSceneFloatTrack* FloatTrack,
        const FString& WidgetFilter,
        const FString& PropertyFilter,
        TArray<TSharedPtr<FJsonObject>>& OutRows)
    {
        if (!Animation || !MovieScene || !FloatTrack)
        {
            return;
        }

        const FString WidgetName = ResolveWidgetNameForBinding(Animation, MovieScene, Binding.GetObjectGuid());
        const FString PropertyPath = FloatTrack->GetPropertyPath().ToString();
        if (!WidgetFilter.IsEmpty() && WidgetName != WidgetFilter)
        {
            return;
        }
        if (!PropertyFilter.IsEmpty() && PropertyPath != PropertyFilter && FloatTrack->GetPropertyName().ToString() != PropertyFilter)
        {
            return;
        }

        const FFrameRate TickResolution = MovieScene->GetTickResolution();
        int32 SectionIndex = 0;
        for (UMovieSceneSection* Section : FloatTrack->GetAllSections())
        {
            const UMovieSceneFloatSection* FloatSection = Cast<UMovieSceneFloatSection>(Section);
            if (!FloatSection)
            {
                ++SectionIndex;
                continue;
            }

            const FMovieSceneFloatChannel& Channel = FloatSection->GetChannel();
            const TArrayView<const FFrameNumber> Times = Channel.GetTimes();
            const TArrayView<const FMovieSceneFloatValue> Values = Channel.GetValues();
            const int32 NumKeys = FMath::Min(Times.Num(), Values.Num());
            OutRows.Reserve(OutRows.Num() + NumKeys);
            for (int32 KeyIndex = 0; KeyIndex < NumKeys; ++KeyIndex)
            {
                TSharedPtr<FJsonObject> Row = MakeShared<FJsonObject>();
                Row->SetStringField(TEXT("row_type"), TEXT("property_key"));
                Row->SetStringField(TEXT("value_type"), TEXT("float"));
                Row->SetNumberField(TEXT("frame"), Times[KeyIndex].Value);
                Row->SetNumberField(TEXT("time"), FrameToSeconds(Times[KeyIndex], TickResolution));
                Row->SetStringField(TEXT("binding_guid"), GuidToString(Binding.GetObjectGuid()));
                Row->SetStringField(TEXT("widget_name"), WidgetName);
                Row->SetStringField(TEXT("track_class"), FloatTrack->GetClass()->GetName());
                Row->SetStringField(TEXT("property_name"), FloatTrack->GetPropertyName().ToString());
                Row->SetStringField(TEXT("property_path"), PropertyPath);
                Row->SetStringField(TEXT("channel_name"), TEXT("float"));
                Row->SetNumberField(TEXT("section_index"), SectionIndex);
                Row->SetNumberField(TEXT("key_index"), KeyIndex);
                Row->SetObjectField(TEXT("key"), MakeValueObject(Values[KeyIndex]));
                AddSectionRange(Row, Section, TickResolution);
                OutRows.Add(Row);
            }
            ++SectionIndex;
        }
    }

    void AppendEventTimelineRows(
        UMovieScene* MovieScene,
        const UMovieSceneEventTrack* EventTrack,
        TArray<TSharedPtr<FJsonObject>>& OutRows)
    {
        if (!MovieScene || !EventTrack)
        {
            return;
        }

        const FFrameRate TickResolution = MovieScene->GetTickResolution();
        int32 SectionIndex = 0;
        for (UMovieSceneSection* Section : EventTrack->GetAllSections())
        {
            const UMovieSceneEventTriggerSection* TriggerSection = Cast<UMovieSceneEventTriggerSection>(Section);
            if (!TriggerSection)
            {
                ++SectionIndex;
                continue;
            }
            const auto EventData = TriggerSection->EventChannel.GetData();
            const TArrayView<const FFrameNumber> Times = EventData.GetTimes();
            const TArrayView<const FMovieSceneEvent> Events = EventData.GetValues();
            const int32 NumKeys = FMath::Min(Times.Num(), Events.Num());
            OutRows.Reserve(OutRows.Num() + NumKeys);
            for (int32 KeyIndex = 0; KeyIndex < NumKeys; ++KeyIndex)
            {
                TSharedPtr<FJsonObject> Row = MakeShared<FJsonObject>();
                Row->SetStringField(TEXT("row_type"), TEXT("event_key"));
                Row->SetStringField(TEXT("value_type"), TEXT("event"));
                Row->SetNumberField(TEXT("frame"), Times[KeyIndex].Value);
                Row->SetNumberField(TEXT("time"), FrameToSeconds(Times[KeyIndex], TickResolution));
                Row->SetStringField(TEXT("track_class"), EventTrack->GetClass()->GetName());
                Row->SetStringField(TEXT("event_name"), Events[KeyIndex].CompiledFunctionName.ToString());
                Row->SetNumberField(TEXT("section_index"), SectionIndex);
                Row->SetNumberField(TEXT("key_index"), KeyIndex);
                AddSectionRange(Row, Section, TickResolution);
                OutRows.Add(Row);
            }
            ++SectionIndex;
        }
    }

    void SortTimelineRows(TArray<TSharedPtr<FJsonObject>>& Rows)
    {
        Rows.Sort([](const TSharedPtr<FJsonObject>& A, const TSharedPtr<FJsonObject>& B)
        {
            double AFrame = 0.0;
            double BFrame = 0.0;
            if (A.IsValid())
            {
                A->TryGetNumberField(TEXT("frame"), AFrame);
            }
            if (B.IsValid())
            {
                B->TryGetNumberField(TEXT("frame"), BFrame);
            }
            if (!FMath::IsNearlyEqual(AFrame, BFrame))
            {
                return AFrame < BFrame;
            }

            FString AType;
            FString BType;
            if (A.IsValid())
            {
                A->TryGetStringField(TEXT("row_type"), AType);
            }
            if (B.IsValid())
            {
                B->TryGetStringField(TEXT("row_type"), BType);
            }
            return AType < BType;
        });
    }

    TArray<TSharedPtr<FJsonValue>> RowsToJsonValues(const TArray<TSharedPtr<FJsonObject>>& Rows, const int32 MaxRows)
    {
        TArray<TSharedPtr<FJsonValue>> Values;
        const int32 Limit = MaxRows > 0 ? FMath::Min(MaxRows, Rows.Num()) : Rows.Num();
        Values.Reserve(Limit);
        for (int32 Index = 0; Index < Limit; ++Index)
        {
            Values.Add(MakeShared<FJsonValueObject>(Rows[Index]));
        }
        return Values;
    }

    TSharedPtr<FJsonObject> MakeAnimationOverview(UWidgetBlueprint* WBP, UWidgetAnimation* Animation)
    {
        TSharedPtr<FJsonObject> Obj = MakeShared<FJsonObject>();
        if (!Animation)
        {
            return Obj;
        }

        UMovieScene* MovieScene = Animation->GetMovieScene();
        Obj->SetStringField(TEXT("name"), GetAnimationReadableName(Animation));
        Obj->SetStringField(TEXT("object_name"), Animation->GetName());
        Obj->SetBoolField(TEXT("read_only"), true);
        Obj->SetNumberField(TEXT("animation_binding_count"), Animation->AnimationBindings.Num());

        TArray<TSharedPtr<FJsonValue>> AnimationBindingRows;
        AnimationBindingRows.Reserve(Animation->AnimationBindings.Num());
        for (const FWidgetAnimationBinding& Binding : Animation->AnimationBindings)
        {
            TSharedPtr<FJsonObject> BindingObj = MakeShared<FJsonObject>();
            BindingObj->SetStringField(TEXT("binding_guid"), GuidToString(Binding.AnimationGuid));
            BindingObj->SetStringField(TEXT("widget_name"), Binding.WidgetName.ToString());
            BindingObj->SetStringField(TEXT("slot_widget_name"), Binding.SlotWidgetName.ToString());
            BindingObj->SetBoolField(TEXT("is_root_widget"), Binding.bIsRootWidget);
            AnimationBindingRows.Add(MakeShared<FJsonValueObject>(BindingObj));
        }
        Obj->SetArrayField(TEXT("animation_bindings"), AnimationBindingRows);

        if (!MovieScene)
        {
            Obj->SetBoolField(TEXT("has_movie_scene"), false);
            return Obj;
        }

        Obj->SetBoolField(TEXT("has_movie_scene"), true);
        const FFrameRate TickResolution = MovieScene->GetTickResolution();
        const FFrameRate DisplayRate = MovieScene->GetDisplayRate();
        Obj->SetStringField(TEXT("tick_resolution"), FString::Printf(TEXT("%d/%d"), TickResolution.Numerator, TickResolution.Denominator));
        Obj->SetStringField(TEXT("display_rate"), FString::Printf(TEXT("%d/%d"), DisplayRate.Numerator, DisplayRate.Denominator));

        const TRange<FFrameNumber> PlaybackRange = MovieScene->GetPlaybackRange();
        if (PlaybackRange.HasLowerBound())
        {
            Obj->SetNumberField(TEXT("start_frame"), PlaybackRange.GetLowerBoundValue().Value);
            Obj->SetNumberField(TEXT("start_time"), FrameToSeconds(PlaybackRange.GetLowerBoundValue(), TickResolution));
        }
        if (PlaybackRange.HasUpperBound())
        {
            Obj->SetNumberField(TEXT("end_frame"), PlaybackRange.GetUpperBoundValue().Value);
            Obj->SetNumberField(TEXT("end_time"), FrameToSeconds(PlaybackRange.GetUpperBoundValue(), TickResolution));
        }

        TArray<TSharedPtr<FJsonValue>> BindingRows;
        TArray<TSharedPtr<FJsonValue>> TrackRows;
        int32 TrackIndex = 0;
        int32 KeyCount = 0;
        const UMovieScene* ConstMovieScene = MovieScene;
        BindingRows.Reserve(ConstMovieScene->GetBindings().Num());
        TrackRows.Reserve(ConstMovieScene->GetBindings().Num() * 2 + MovieScene->GetTracks().Num());
        for (const FMovieSceneBinding& Binding : ConstMovieScene->GetBindings())
        {
            TSharedPtr<FJsonObject> BindingObj = MakeBindingIdentity(Animation, MovieScene, Binding.GetObjectGuid());
            BindingObj->SetNumberField(TEXT("track_count"), Binding.GetTracks().Num());
            BindingRows.Add(MakeShared<FJsonValueObject>(BindingObj));

            for (UMovieSceneTrack* Track : Binding.GetTracks())
            {
                if (const UMovieSceneFloatTrack* FloatTrack = Cast<UMovieSceneFloatTrack>(Track))
                {
                    const int32 TrackKeyCount = CountFloatKeys(FloatTrack);
                    KeyCount += TrackKeyCount;
                    TrackRows.Add(MakeShared<FJsonValueObject>(
                        MakeFloatTrackSummary(Animation, MovieScene, Binding, FloatTrack, TrackIndex)));
                }
                else if (Track)
                {
                    TSharedPtr<FJsonObject> Unsupported = MakeShared<FJsonObject>();
                    Unsupported->SetStringField(TEXT("track_kind"), TEXT("unsupported_binding_track"));
                    Unsupported->SetStringField(TEXT("track_class"), Track->GetClass()->GetName());
                    Unsupported->SetStringField(TEXT("display_name"), Track->GetDisplayName().ToString());
                    Unsupported->SetNumberField(TEXT("track_index"), TrackIndex);
                    Unsupported->SetObjectField(TEXT("binding"), MakeBindingIdentity(Animation, MovieScene, Binding.GetObjectGuid()));
                    Unsupported->SetBoolField(TEXT("timeline_supported"), false);
                    Unsupported->SetBoolField(TEXT("time_slice_supported"), false);
                    TrackRows.Add(MakeShared<FJsonValueObject>(Unsupported));
                }
                ++TrackIndex;
            }
        }

        for (UMovieSceneTrack* Track : MovieScene->GetTracks())
        {
            if (const UMovieSceneEventTrack* EventTrack = Cast<UMovieSceneEventTrack>(Track))
            {
                const int32 TrackKeyCount = CountEventKeys(EventTrack);
                KeyCount += TrackKeyCount;
                TrackRows.Add(MakeShared<FJsonValueObject>(MakeEventTrackSummary(EventTrack, TrackIndex)));
            }
            else if (Track)
            {
                TSharedPtr<FJsonObject> Unsupported = MakeShared<FJsonObject>();
                Unsupported->SetStringField(TEXT("track_kind"), TEXT("unsupported_master_track"));
                Unsupported->SetStringField(TEXT("track_class"), Track->GetClass()->GetName());
                Unsupported->SetStringField(TEXT("display_name"), Track->GetDisplayName().ToString());
                Unsupported->SetNumberField(TEXT("track_index"), TrackIndex);
                Unsupported->SetBoolField(TEXT("timeline_supported"), false);
                Unsupported->SetBoolField(TEXT("time_slice_supported"), false);
                TrackRows.Add(MakeShared<FJsonValueObject>(Unsupported));
            }
            ++TrackIndex;
        }

        TArray<TSharedPtr<FJsonValue>> DelegateRows;
        AppendDelegateBindingRows(WBP, GetAnimationReadableName(Animation), DelegateRows);

        Obj->SetArrayField(TEXT("movie_scene_bindings"), BindingRows);
        Obj->SetArrayField(TEXT("tracks"), TrackRows);
        Obj->SetArrayField(TEXT("delegate_bindings"), DelegateRows);
        Obj->SetNumberField(TEXT("movie_scene_binding_count"), BindingRows.Num());
        Obj->SetNumberField(TEXT("track_count"), TrackRows.Num());
        Obj->SetNumberField(TEXT("key_count"), KeyCount);
        Obj->SetNumberField(TEXT("delegate_binding_count"), DelegateRows.Num());
        return Obj;
    }

    enum class EAnimationDeltaOpKind : uint8
    {
        UpsertFloatKey,
        DeleteFloatKey
    };

    struct FAnimationDeltaOp
    {
        EAnimationDeltaOpKind Kind = EAnimationDeltaOpKind::UpsertFloatKey;
        int32 OperationIndex = INDEX_NONE;
        FString OperationName;
        FString WidgetName;
        FString BindingGuidText;
        FGuid BindingGuid;
        FString PropertyPath;
        FString Component;
        double TimeSeconds = 0.0;
        FFrameNumber Frame;
        double Value = 0.0;
        ERichCurveInterpMode InterpMode = RCIM_Linear;
        bool bConfirmDelete = false;
        bool bHasArriveTangent = false;
        bool bHasLeaveTangent = false;
        bool bHasArriveWeight = false;
        bool bHasLeaveWeight = false;
        double ArriveTangent = 0.0;
        double LeaveTangent = 0.0;
        double ArriveWeight = 0.0;
        double LeaveWeight = 0.0;
    };

    TArray<TSharedPtr<FJsonValue>> MakeAnimationDeltaExternalAliases()
    {
        TArray<TSharedPtr<FJsonValue>> Aliases;
        Aliases.Reserve(4);
        Aliases.Add(MakeShared<FJsonValueString>(TEXT("animation_append_widget_tracks")));
        Aliases.Add(MakeShared<FJsonValueString>(TEXT("animation_append_time_slice")));
        Aliases.Add(MakeShared<FJsonValueString>(TEXT("animation_delete_widget_keys")));
        Aliases.Add(MakeShared<FJsonValueString>(TEXT("set_property_keys")));
        return Aliases;
    }

    FString NormalizeAnimationToken(FString Value)
    {
        Value.TrimStartAndEndInline();
        Value.ToLowerInline();
        Value.ReplaceInline(TEXT("-"), TEXT("_"));
        Value.ReplaceInline(TEXT("."), TEXT("_"));
        Value.ReplaceInline(TEXT(" "), TEXT("_"));
        return Value;
    }

    bool TryCanonicalKnownFloatTrackPath(const FString& InProperty, FString& OutPropertyPath)
    {
        const FString Token = NormalizeAnimationToken(InProperty);
        if (Token == TEXT("renderopacity") || Token == TEXT("render_opacity") || Token == TEXT("opacity"))
        {
            OutPropertyPath = TEXT("RenderOpacity");
            return true;
        }

        struct FKnownPath
        {
            const TCHAR* Token;
            const TCHAR* Path;
        };

        static const FKnownPath KnownPaths[] = {
            { TEXT("rendertransform_translation_x"), TEXT("RenderTransform.Translation.X") },
            { TEXT("render_transform_translation_x"), TEXT("RenderTransform.Translation.X") },
            { TEXT("translation_x"), TEXT("RenderTransform.Translation.X") },
            { TEXT("tx"), TEXT("RenderTransform.Translation.X") },
            { TEXT("rendertransform_translation_y"), TEXT("RenderTransform.Translation.Y") },
            { TEXT("render_transform_translation_y"), TEXT("RenderTransform.Translation.Y") },
            { TEXT("translation_y"), TEXT("RenderTransform.Translation.Y") },
            { TEXT("ty"), TEXT("RenderTransform.Translation.Y") },
            { TEXT("rendertransform_angle"), TEXT("RenderTransform.Angle") },
            { TEXT("render_transform_angle"), TEXT("RenderTransform.Angle") },
            { TEXT("angle"), TEXT("RenderTransform.Angle") },
            { TEXT("rotation"), TEXT("RenderTransform.Angle") },
            { TEXT("rendertransform_scale_x"), TEXT("RenderTransform.Scale.X") },
            { TEXT("render_transform_scale_x"), TEXT("RenderTransform.Scale.X") },
            { TEXT("scale_x"), TEXT("RenderTransform.Scale.X") },
            { TEXT("sx"), TEXT("RenderTransform.Scale.X") },
            { TEXT("rendertransform_scale_y"), TEXT("RenderTransform.Scale.Y") },
            { TEXT("render_transform_scale_y"), TEXT("RenderTransform.Scale.Y") },
            { TEXT("scale_y"), TEXT("RenderTransform.Scale.Y") },
            { TEXT("sy"), TEXT("RenderTransform.Scale.Y") },
            { TEXT("rendertransform_shear_x"), TEXT("RenderTransform.Shear.X") },
            { TEXT("render_transform_shear_x"), TEXT("RenderTransform.Shear.X") },
            { TEXT("shear_x"), TEXT("RenderTransform.Shear.X") },
            { TEXT("shx"), TEXT("RenderTransform.Shear.X") },
            { TEXT("rendertransform_shear_y"), TEXT("RenderTransform.Shear.Y") },
            { TEXT("render_transform_shear_y"), TEXT("RenderTransform.Shear.Y") },
            { TEXT("shear_y"), TEXT("RenderTransform.Shear.Y") },
            { TEXT("shy"), TEXT("RenderTransform.Shear.Y") },
            { TEXT("colorandopacity_r"), TEXT("ColorAndOpacity.R") },
            { TEXT("color_and_opacity_r"), TEXT("ColorAndOpacity.R") },
            { TEXT("color_r"), TEXT("ColorAndOpacity.R") },
            { TEXT("r"), TEXT("ColorAndOpacity.R") },
            { TEXT("red"), TEXT("ColorAndOpacity.R") },
            { TEXT("colorandopacity_g"), TEXT("ColorAndOpacity.G") },
            { TEXT("color_and_opacity_g"), TEXT("ColorAndOpacity.G") },
            { TEXT("color_g"), TEXT("ColorAndOpacity.G") },
            { TEXT("g"), TEXT("ColorAndOpacity.G") },
            { TEXT("green"), TEXT("ColorAndOpacity.G") },
            { TEXT("colorandopacity_b"), TEXT("ColorAndOpacity.B") },
            { TEXT("color_and_opacity_b"), TEXT("ColorAndOpacity.B") },
            { TEXT("color_b"), TEXT("ColorAndOpacity.B") },
            { TEXT("b"), TEXT("ColorAndOpacity.B") },
            { TEXT("blue"), TEXT("ColorAndOpacity.B") },
            { TEXT("colorandopacity_a"), TEXT("ColorAndOpacity.A") },
            { TEXT("color_and_opacity_a"), TEXT("ColorAndOpacity.A") },
            { TEXT("color_a"), TEXT("ColorAndOpacity.A") },
            { TEXT("a"), TEXT("ColorAndOpacity.A") },
            { TEXT("alpha"), TEXT("ColorAndOpacity.A") },
        };

        for (const FKnownPath& Known : KnownPaths)
        {
            if (Token == Known.Token)
            {
                OutPropertyPath = Known.Path;
                return true;
            }
        }

        return false;
    }

    bool TryNormalizeFloatDeltaPropertyPath(
        const FString& InProperty,
        const FString& InComponent,
        FString& OutPropertyPath,
        FString& OutError)
    {
        FString Property = InProperty;
        Property.TrimStartAndEndInline();
        if (Property.IsEmpty())
        {
            OutPropertyPath.Reset();
            return true;
        }

        if (TryCanonicalKnownFloatTrackPath(Property, OutPropertyPath))
        {
            return true;
        }

        const FString PropertyToken = NormalizeAnimationToken(Property);
        const FString ComponentToken = NormalizeAnimationToken(InComponent);

        if (PropertyToken == TEXT("transform") || PropertyToken == TEXT("rendertransform") || PropertyToken == TEXT("render_transform"))
        {
            if (ComponentToken.IsEmpty())
            {
                OutError = TEXT("property 'transform' requires component one of: tx, ty, angle, sx, sy, shx, shy");
                return false;
            }
            if (TryCanonicalKnownFloatTrackPath(ComponentToken, OutPropertyPath)
                && OutPropertyPath.StartsWith(TEXT("RenderTransform.")))
            {
                return true;
            }
            OutError = TEXT("property 'transform' component must be one of: tx, ty, angle, sx, sy, shx, shy");
            return false;
        }

        if (PropertyToken == TEXT("color") || PropertyToken == TEXT("colorandopacity") || PropertyToken == TEXT("color_and_opacity"))
        {
            if (ComponentToken.IsEmpty())
            {
                OutError = TEXT("property 'color' requires component one of: r, g, b, a");
                return false;
            }
            if (TryCanonicalKnownFloatTrackPath(ComponentToken, OutPropertyPath)
                && OutPropertyPath.StartsWith(TEXT("ColorAndOpacity.")))
            {
                return true;
            }
            OutError = TEXT("property 'color' component must be one of: r, g, b, a");
            return false;
        }

        OutPropertyPath = Property;
        return true;
    }

    FName FloatTrackPropertyNameFromPath(const FString& PropertyPath)
    {
        if (PropertyPath == TEXT("RenderOpacity"))
        {
            return FName(TEXT("RenderOpacity"));
        }
        if (PropertyPath == TEXT("RenderTransform.Translation.X"))
        {
            return FName(TEXT("Translation X"));
        }
        if (PropertyPath == TEXT("RenderTransform.Translation.Y"))
        {
            return FName(TEXT("Translation Y"));
        }
        if (PropertyPath == TEXT("RenderTransform.Angle"))
        {
            return FName(TEXT("Angle"));
        }
        if (PropertyPath == TEXT("RenderTransform.Scale.X"))
        {
            return FName(TEXT("Scale X"));
        }
        if (PropertyPath == TEXT("RenderTransform.Scale.Y"))
        {
            return FName(TEXT("Scale Y"));
        }
        if (PropertyPath == TEXT("RenderTransform.Shear.X"))
        {
            return FName(TEXT("Shear X"));
        }
        if (PropertyPath == TEXT("RenderTransform.Shear.Y"))
        {
            return FName(TEXT("Shear Y"));
        }
        if (PropertyPath == TEXT("ColorAndOpacity.R"))
        {
            return FName(TEXT("Color R"));
        }
        if (PropertyPath == TEXT("ColorAndOpacity.G"))
        {
            return FName(TEXT("Color G"));
        }
        if (PropertyPath == TEXT("ColorAndOpacity.B"))
        {
            return FName(TEXT("Color B"));
        }
        if (PropertyPath == TEXT("ColorAndOpacity.A"))
        {
            return FName(TEXT("Color A"));
        }

        FString Tail = PropertyPath;
        int32 DotIndex = INDEX_NONE;
        if (PropertyPath.FindLastChar(TEXT('.'), DotIndex) && DotIndex + 1 < PropertyPath.Len())
        {
            Tail = PropertyPath.Mid(DotIndex + 1);
        }
        Tail.ReplaceInline(TEXT("_"), TEXT(" "));
        Tail.TrimStartAndEndInline();
        return Tail.IsEmpty() ? FName(*PropertyPath) : FName(*Tail);
    }

    FString AnimationDeltaOpKindToString(const EAnimationDeltaOpKind Kind)
    {
        switch (Kind)
        {
        case EAnimationDeltaOpKind::UpsertFloatKey:
            return TEXT("upsert_float_key");
        case EAnimationDeltaOpKind::DeleteFloatKey:
            return TEXT("delete_float_key");
        default:
            return TEXT("unknown");
        }
    }

    bool TryParseAnimationDeltaOpName(
        const FString& InOp,
        EAnimationDeltaOpKind& OutKind,
        FString& OutCanonicalOp)
    {
        FString Op = InOp;
        Op.TrimStartAndEndInline();
        Op.ToLowerInline();

        if (Op == TEXT("upsert_float_key")
            || Op == TEXT("merge_float_key")
            || Op == TEXT("set_float_key"))
        {
            OutKind = EAnimationDeltaOpKind::UpsertFloatKey;
            OutCanonicalOp = TEXT("upsert_float_key");
            return true;
        }
        if (Op == TEXT("delete_float_key")
            || Op == TEXT("remove_float_key"))
        {
            OutKind = EAnimationDeltaOpKind::DeleteFloatKey;
            OutCanonicalOp = TEXT("delete_float_key");
            return true;
        }
        return false;
    }

    bool TryParseDeltaInterpMode(const TSharedPtr<FJsonObject>& Obj, ERichCurveInterpMode& OutMode, FString& OutError)
    {
        FString Interp;
        if (!Obj.IsValid() || !Obj->TryGetStringField(TEXT("interp"), Interp))
        {
            OutMode = RCIM_Linear;
            return true;
        }

        Interp.TrimStartAndEndInline();
        Interp.ToLowerInline();
        if (Interp == TEXT("linear"))
        {
            OutMode = RCIM_Linear;
            return true;
        }
        if (Interp == TEXT("constant"))
        {
            OutMode = RCIM_Constant;
            return true;
        }
        if (Interp == TEXT("cubic"))
        {
            OutMode = RCIM_Cubic;
            return true;
        }

        OutError = TEXT("interp must be one of: linear, constant, cubic");
        return false;
    }

    bool TryGetOptionalNumberField(
        const TSharedPtr<FJsonObject>& Obj,
        const TCHAR* FieldName,
        bool& bOutHasValue,
        double& OutValue,
        FString& OutError)
    {
        bOutHasValue = false;
        if (!Obj.IsValid() || !Obj->HasField(FieldName))
        {
            return true;
        }
        if (!Obj->TryGetNumberField(FieldName, OutValue))
        {
            OutError = FString::Printf(TEXT("%s must be a number"), FieldName);
            return false;
        }
        bOutHasValue = true;
        return true;
    }

    FMovieSceneFloatValue MakeDeltaFloatValue(const FAnimationDeltaOp& Op)
    {
        FMovieSceneFloatValue FloatValue(static_cast<float>(Op.Value));
        FloatValue.InterpMode = Op.InterpMode;
        const bool bHasTangentData =
            Op.bHasArriveTangent || Op.bHasLeaveTangent || Op.bHasArriveWeight || Op.bHasLeaveWeight;
        FloatValue.TangentMode = bHasTangentData ? RCTM_User : RCTM_Auto;
        if (Op.bHasArriveTangent)
        {
            FloatValue.Tangent.ArriveTangent = static_cast<float>(Op.ArriveTangent);
        }
        if (Op.bHasLeaveTangent)
        {
            FloatValue.Tangent.LeaveTangent = static_cast<float>(Op.LeaveTangent);
        }
        if (Op.bHasArriveWeight)
        {
            FloatValue.Tangent.ArriveTangentWeight = static_cast<float>(Op.ArriveWeight);
        }
        if (Op.bHasLeaveWeight)
        {
            FloatValue.Tangent.LeaveTangentWeight = static_cast<float>(Op.LeaveWeight);
        }
        if (Op.bHasArriveWeight && Op.bHasLeaveWeight)
        {
            FloatValue.Tangent.TangentWeightMode = RCTWM_WeightedBoth;
        }
        else if (Op.bHasArriveWeight)
        {
            FloatValue.Tangent.TangentWeightMode = RCTWM_WeightedArrive;
        }
        else if (Op.bHasLeaveWeight)
        {
            FloatValue.Tangent.TangentWeightMode = RCTWM_WeightedLeave;
        }
        return FloatValue;
    }

    bool TryParseAnimationDeltaOperation(
        const TSharedPtr<FJsonObject>& Obj,
        const int32 OperationIndex,
        const FFrameRate& TickResolution,
        FAnimationDeltaOp& OutOp,
        FMonolithActionResult& OutError)
    {
        if (!Obj.IsValid())
        {
            OutError = FMonolithActionResult::Error(
                FString::Printf(TEXT("operations[%d] must be an object"), OperationIndex),
                -32602);
            return false;
        }

        FString RawOpName;
        if (!Obj->TryGetStringField(TEXT("op"), RawOpName) || RawOpName.IsEmpty())
        {
            OutError = FMonolithActionResult::Error(
                FString::Printf(TEXT("operations[%d].op is required"), OperationIndex),
                -32602);
            return false;
        }

        FString CanonicalOp;
        EAnimationDeltaOpKind Kind = EAnimationDeltaOpKind::UpsertFloatKey;
        if (!TryParseAnimationDeltaOpName(RawOpName, Kind, CanonicalOp))
        {
            OutError = FMonolithActionResult::Error(
                FString::Printf(
                    TEXT("operations[%d].op '%s' is unsupported; supported ops: upsert_float_key, delete_float_key"),
                    OperationIndex,
                    *RawOpName),
                -32602);
            return false;
        }

        FAnimationDeltaOp Op;
        Op.Kind = Kind;
        Op.OperationName = CanonicalOp;
        Op.OperationIndex = OperationIndex;
        Obj->TryGetStringField(TEXT("widget_name"), Op.WidgetName);
        Obj->TryGetStringField(TEXT("binding_guid"), Op.BindingGuidText);
        Obj->TryGetStringField(TEXT("component"), Op.Component);
        if (Op.WidgetName.IsEmpty() && Op.BindingGuidText.IsEmpty())
        {
            OutError = FMonolithActionResult::Error(
                FString::Printf(TEXT("operations[%d] requires widget_name or binding_guid"), OperationIndex),
                -32602);
            return false;
        }
        if (!Op.BindingGuidText.IsEmpty() && !FGuid::Parse(Op.BindingGuidText, Op.BindingGuid))
        {
            OutError = FMonolithActionResult::Error(
                FString::Printf(TEXT("operations[%d].binding_guid is not a valid GUID"), OperationIndex),
                -32602);
            return false;
        }

        FString Property;
        if (!Obj->TryGetStringField(TEXT("property_path"), Property))
        {
            Obj->TryGetStringField(TEXT("property"), Property);
        }
        FString NormalizeError;
        if (!TryNormalizeFloatDeltaPropertyPath(Property, Op.Component, Op.PropertyPath, NormalizeError))
        {
            OutError = FMonolithActionResult::Error(
                FString::Printf(TEXT("operations[%d].%s"), OperationIndex, *NormalizeError),
                -32602);
            return false;
        }
        if (Op.PropertyPath.IsEmpty())
        {
            OutError = FMonolithActionResult::Error(
                FString::Printf(TEXT("operations[%d] requires property_path or property"), OperationIndex),
                -32602);
            return false;
        }

        if (!Obj->TryGetNumberField(TEXT("time"), Op.TimeSeconds))
        {
            OutError = FMonolithActionResult::Error(
                FString::Printf(TEXT("operations[%d].time is required"), OperationIndex),
                -32602);
            return false;
        }
        if (Op.TimeSeconds < 0.0)
        {
            OutError = FMonolithActionResult::Error(
                FString::Printf(TEXT("operations[%d].time must be >= 0"), OperationIndex),
                -32602);
            return false;
        }
        Op.Frame = TickResolution.AsFrameNumber(Op.TimeSeconds);

        if (Kind == EAnimationDeltaOpKind::UpsertFloatKey)
        {
            if (!Obj->TryGetNumberField(TEXT("value"), Op.Value))
            {
                OutError = FMonolithActionResult::Error(
                    FString::Printf(TEXT("operations[%d].value is required for upsert_float_key"), OperationIndex),
                    -32602);
                return false;
            }

            FString ParseError;
            if (!TryParseDeltaInterpMode(Obj, Op.InterpMode, ParseError))
            {
                OutError = FMonolithActionResult::Error(
                    FString::Printf(TEXT("operations[%d].%s"), OperationIndex, *ParseError),
                    -32602);
                return false;
            }

            if (!TryGetOptionalNumberField(Obj, TEXT("arrive_tangent"), Op.bHasArriveTangent, Op.ArriveTangent, ParseError)
                || !TryGetOptionalNumberField(Obj, TEXT("leave_tangent"), Op.bHasLeaveTangent, Op.LeaveTangent, ParseError)
                || !TryGetOptionalNumberField(Obj, TEXT("arrive_weight"), Op.bHasArriveWeight, Op.ArriveWeight, ParseError)
                || !TryGetOptionalNumberField(Obj, TEXT("leave_weight"), Op.bHasLeaveWeight, Op.LeaveWeight, ParseError))
            {
                OutError = FMonolithActionResult::Error(
                    FString::Printf(TEXT("operations[%d].%s"), OperationIndex, *ParseError),
                    -32602);
                return false;
            }
        }

        Obj->TryGetBoolField(TEXT("confirm_delete"), Op.bConfirmDelete);
        OutOp = Op;
        return true;
    }

    bool IsDeleteDeltaOp(const FAnimationDeltaOp& Op)
    {
        return Op.Kind == EAnimationDeltaOpKind::DeleteFloatKey;
    }

    FGuid FindExistingBindingGuidForWidget(
        const UWidgetAnimation* Animation,
        UMovieScene* MovieScene,
        const FString& WidgetName)
    {
        if (!Animation || !MovieScene || WidgetName.IsEmpty())
        {
            return FGuid();
        }

        const FName WidgetFName(*WidgetName);
        for (const FWidgetAnimationBinding& Binding : Animation->AnimationBindings)
        {
            if (Binding.WidgetName == WidgetFName)
            {
                return Binding.AnimationGuid;
            }
        }

        const UMovieScene* ConstMovieScene = MovieScene;
        for (const FMovieSceneBinding& Binding : ConstMovieScene->GetBindings())
        {
            if (ResolveBindingName(MovieScene, Binding.GetObjectGuid()) == WidgetName)
            {
                return Binding.GetObjectGuid();
            }
        }
        return FGuid();
    }

    UMovieSceneFloatTrack* FindFloatTrackByPath(
        UMovieScene* MovieScene,
        const FGuid& BindingGuid,
        const FString& PropertyPath)
    {
        if (!MovieScene || !BindingGuid.IsValid())
        {
            return nullptr;
        }

        const FMovieSceneBinding* Binding = static_cast<const UMovieScene*>(MovieScene)->FindBinding(BindingGuid);
        if (!Binding)
        {
            return nullptr;
        }

        for (UMovieSceneTrack* Track : Binding->GetTracks())
        {
            UMovieSceneFloatTrack* FloatTrack = Cast<UMovieSceneFloatTrack>(Track);
            if (FloatTrack && FloatTrack->GetPropertyPath().ToString() == PropertyPath)
            {
                return FloatTrack;
            }
        }
        return nullptr;
    }

    void ExpandSectionRangeToFrame(UMovieSceneSection* Section, const FFrameNumber& Frame)
    {
        if (!Section)
        {
            return;
        }
        const TRange<FFrameNumber> KeyRange(Frame, Frame + 1);
        Section->SetRange(TRange<FFrameNumber>::Hull(Section->GetRange(), KeyRange));
    }

    void ExpandPlaybackRangeToFrame(UMovieScene* MovieScene, const FFrameNumber& Frame)
    {
        if (!MovieScene)
        {
            return;
        }
        const TRange<FFrameNumber> KeyRange(Frame, Frame + 1);
        MovieScene->SetPlaybackRange(TRange<FFrameNumber>::Hull(MovieScene->GetPlaybackRange(), KeyRange));
    }

    UMovieSceneFloatSection* FindOrCreateFloatSectionForFrame(
        UMovieSceneFloatTrack* FloatTrack,
        const FFrameNumber& Frame,
        const bool bCreate)
    {
        if (!FloatTrack)
        {
            return nullptr;
        }

        for (UMovieSceneSection* Section : FloatTrack->GetAllSections())
        {
            UMovieSceneFloatSection* FloatSection = Cast<UMovieSceneFloatSection>(Section);
            if (FloatSection && IsFrameInSection(FloatSection, Frame))
            {
                return FloatSection;
            }
        }

        for (UMovieSceneSection* Section : FloatTrack->GetAllSections())
        {
            if (UMovieSceneFloatSection* FloatSection = Cast<UMovieSceneFloatSection>(Section))
            {
                if (bCreate)
                {
                    FloatSection->Modify();
                    ExpandSectionRangeToFrame(FloatSection, Frame);
                }
                return FloatSection;
            }
        }

        if (!bCreate)
        {
            return nullptr;
        }

        UMovieSceneFloatSection* Section = Cast<UMovieSceneFloatSection>(FloatTrack->CreateNewSection());
        if (!Section)
        {
            return nullptr;
        }
        Section->SetRange(TRange<FFrameNumber>(Frame, Frame + 1));
        FloatTrack->AddSection(*Section);
        return Section;
    }

    UMovieSceneFloatTrack* FindOrCreateDeltaFloatTrack(
        UMovieScene* MovieScene,
        const FGuid& BindingGuid,
        const FString& PropertyPath,
        const FFrameNumber& Frame,
        bool& bOutTrackCreated,
        bool& bOutSectionCreated)
    {
        bOutTrackCreated = false;
        bOutSectionCreated = false;
        if (!MovieScene || !BindingGuid.IsValid())
        {
            return nullptr;
        }

        UMovieSceneFloatTrack* FloatTrack = FindFloatTrackByPath(MovieScene, BindingGuid, PropertyPath);
        if (!FloatTrack)
        {
            FloatTrack = MovieScene->AddTrack<UMovieSceneFloatTrack>(BindingGuid);
            if (!FloatTrack)
            {
                return nullptr;
            }
            FloatTrack->SetPropertyNameAndPath(FloatTrackPropertyNameFromPath(PropertyPath), *PropertyPath);
            bOutTrackCreated = true;
        }

        const int32 PreviousSectionCount = FloatTrack->GetAllSections().Num();
        UMovieSceneFloatSection* Section = FindOrCreateFloatSectionForFrame(FloatTrack, Frame, true);
        bOutSectionCreated = Section && FloatTrack->GetAllSections().Num() > PreviousSectionCount;
        return Section ? FloatTrack : nullptr;
    }

    int32 FindFloatKeyIndex(UMovieSceneFloatSection* Section, const FFrameNumber& Frame)
    {
        if (!Section)
        {
            return INDEX_NONE;
        }
        return Section->GetChannel().GetData().FindKey(Frame);
    }

    TSharedPtr<FJsonObject> MakeDeltaOperationRow(
        const FAnimationDeltaOp& Op,
        const FString& Status,
        const FGuid& BindingGuid,
        const bool bHadExistingKey,
        const int32 DeletedKeyCount,
        const bool bWouldCreateBinding,
        const bool bWouldCreateTrack,
        const bool bWouldCreateSection)
    {
        TSharedPtr<FJsonObject> Row = MakeShared<FJsonObject>();
        Row->SetNumberField(TEXT("operation_index"), Op.OperationIndex);
        Row->SetStringField(TEXT("op"), AnimationDeltaOpKindToString(Op.Kind));
        Row->SetStringField(TEXT("status"), Status);
        Row->SetNumberField(TEXT("frame"), Op.Frame.Value);
        Row->SetNumberField(TEXT("time"), Op.TimeSeconds);
        Row->SetStringField(TEXT("widget_name"), Op.WidgetName);
        Row->SetStringField(TEXT("binding_guid"), BindingGuid.IsValid() ? GuidToString(BindingGuid) : Op.BindingGuidText);
        Row->SetStringField(TEXT("property_path"), Op.PropertyPath);
        if (!Op.Component.IsEmpty())
        {
            Row->SetStringField(TEXT("component"), Op.Component);
        }
        Row->SetBoolField(TEXT("had_existing_key"), bHadExistingKey);
        Row->SetNumberField(TEXT("deleted_key_count"), DeletedKeyCount);
        Row->SetBoolField(TEXT("would_create_binding"), bWouldCreateBinding);
        Row->SetBoolField(TEXT("would_create_track"), bWouldCreateTrack);
        Row->SetBoolField(TEXT("would_create_section"), bWouldCreateSection);
        if (Op.Kind == EAnimationDeltaOpKind::UpsertFloatKey)
        {
            Row->SetNumberField(TEXT("value"), Op.Value);
            Row->SetStringField(TEXT("interp"), InterpModeToString(Op.InterpMode));
        }
        return Row;
    }
}

void FMonolithUIAnimationActions::RegisterActions(FMonolithToolRegistry& Registry)
{
    Registry.RegisterAction(
        TEXT("ui"), TEXT("list_animations"),
        TEXT("List all UWidgetAnimation assets on a Widget Blueprint"),
        FMonolithActionHandler::CreateStatic(&HandleListAnimations),
        FParamSchemaBuilder()
            .RequiredAssetPath(TEXT("asset_path"), TEXT("Widget Blueprint asset path"))
            .Build()
    );

    Registry.RegisterAction(
        TEXT("ui"), TEXT("get_animation_details"),
        TEXT("Get tracks and keyframes for a specific animation on a Widget Blueprint"),
        FMonolithActionHandler::CreateStatic(&HandleGetAnimationDetails),
        FParamSchemaBuilder()
            .RequiredAssetPath(TEXT("asset_path"), TEXT("Widget Blueprint asset path"))
            .Required(TEXT("animation_name"), TEXT("string"), TEXT("Name of the UWidgetAnimation"))
            .Build()
    );

    Registry.RegisterAction(
        TEXT("ui"), TEXT("get_animation_overview"),
        TEXT("Read-only compact UMG animation inventory: timing, bindings, tracks, key counts, event tracks, and delegate binding rows"),
        FMonolithActionHandler::CreateStatic(&HandleGetAnimationOverview),
        FParamSchemaBuilder()
            .RequiredAssetPath(TEXT("asset_path"), TEXT("Widget Blueprint asset path"))
            .Optional(TEXT("animation_name"), TEXT("string"), TEXT("Name/display label of the UWidgetAnimation. Required unless include_all=true."))
            .Optional(TEXT("include_all"), TEXT("boolean"), TEXT("Return every animation on the Widget Blueprint (default false)"), TEXT("false"))
            .Build()
    );

    Registry.RegisterAction(
        TEXT("ui"), TEXT("get_animation_timeline"),
        TEXT("Read-only sorted key/event timeline for a UMG animation, filtered by widget or property path"),
        FMonolithActionHandler::CreateStatic(&HandleGetAnimationTimeline),
        FParamSchemaBuilder()
            .RequiredAssetPath(TEXT("asset_path"), TEXT("Widget Blueprint asset path"))
            .Required(TEXT("animation_name"), TEXT("string"), TEXT("Name/display label of the UWidgetAnimation"))
            .Optional(TEXT("widget_name"), TEXT("string"), TEXT("Only include rows for this widget binding"))
            .Optional(TEXT("property_path"), TEXT("string"), TEXT("Only include a property path/name such as RenderOpacity"))
            .Optional(TEXT("include_events"), TEXT("boolean"), TEXT("Include master event-track rows (default true)"), TEXT("true"))
            .Optional(TEXT("max_rows"), TEXT("integer"), TEXT("Maximum rows to return; <=0 means no truncation (default 1000)"), TEXT("1000"))
            .Build()
    );

    Registry.RegisterAction(
        TEXT("ui"), TEXT("get_animation_time_slice"),
        TEXT("Read-only sampled values for continuous float tracks at time/times plus exact-frame event matches"),
        FMonolithActionHandler::CreateStatic(&HandleGetAnimationTimeSlice),
        FParamSchemaBuilder()
            .RequiredAssetPath(TEXT("asset_path"), TEXT("Widget Blueprint asset path"))
            .Required(TEXT("animation_name"), TEXT("string"), TEXT("Name/display label of the UWidgetAnimation"))
            .Optional(TEXT("time"), TEXT("number"), TEXT("Single time in seconds"))
            .Optional(TEXT("times"), TEXT("array"), TEXT("Array of times in seconds"))
            .Optional(TEXT("widget_name"), TEXT("string"), TEXT("Only include sampled rows for this widget binding"))
            .Optional(TEXT("property_path"), TEXT("string"), TEXT("Only include a property path/name such as RenderOpacity"))
            .Optional(TEXT("event_tolerance_frames"), TEXT("integer"), TEXT("Frame tolerance for event matches (default 0 exact only)"), TEXT("0"))
            .Build()
    );

    Registry.RegisterAction(
        TEXT("ui"), TEXT("apply_animation_delta"),
        TEXT("Confirm-gated delta edit for existing UMG animations. Supports scalar float-key upsert/delete without resetting sections, including canonical opacity plus transform/color component paths; writes require dry_run=false and confirm=true, deletes additionally require confirm_delete=true."),
        FMonolithActionHandler::CreateStatic(&HandleApplyAnimationDelta),
        FParamSchemaBuilder()
            .RequiredAssetPath(TEXT("asset_path"), TEXT("Widget Blueprint asset path"))
            .Required(TEXT("animation_name"), TEXT("string"), TEXT("Name/display label of the existing UWidgetAnimation"))
            .Required(TEXT("operations"), TEXT("array"), TEXT("Array of delta operations. Ops: upsert_float_key/delete_float_key with widget_name or binding_guid, property_path/property, optional component for transform/color, time, and value for upsert."))
            .Optional(TEXT("dry_run"), TEXT("boolean"), TEXT("Plan only; default true. Set false with confirm=true to mutate."), TEXT("true"))
            .Optional(TEXT("confirm"), TEXT("boolean"), TEXT("Required true when dry_run=false."), TEXT("false"))
            .Optional(TEXT("confirm_delete"), TEXT("boolean"), TEXT("Required true for delete operations, globally or per operation."), TEXT("false"))
            .Optional(TEXT("compile"), TEXT("boolean"), TEXT("Compile the Widget Blueprint after an applied mutation (default true)."), TEXT("true"))
            .Optional(TEXT("read_back"), TEXT("boolean"), TEXT("Include post-plan/post-write animation overview read-back (default true)."), TEXT("true"))
            .Build()
    );

    Registry.RegisterAction(
        TEXT("ui"), TEXT("remap_animation_binding"),
        TEXT("Confirm-gated remap of one existing UWidgetAnimation binding from a removed or replaced widget name to a resident WidgetTree target while preserving its binding GUID, MovieScene tracks, sections, and keys. Slot-widget bindings are rejected. Dry-run is the default; writes require dry_run=false and confirm=true."),
        FMonolithActionHandler::CreateStatic(&HandleRemapAnimationBinding),
        FParamSchemaBuilder()
            .RequiredAssetPath(TEXT("asset_path"), TEXT("Widget Blueprint asset path"))
            .Required(TEXT("animation_name"), TEXT("string"), TEXT("Name/display label of the existing UWidgetAnimation"))
            .Required(TEXT("from_widget_name"), TEXT("string"), TEXT("Current widget name stored in the animation binding; the widget may already be absent from the WidgetTree"))
            .Required(TEXT("to_widget_name"), TEXT("string"), TEXT("Resident replacement widget name in the WidgetTree"))
            .Optional(TEXT("dry_run"), TEXT("boolean"), TEXT("Plan only; default true. Set false with confirm=true to mutate."), TEXT("true"))
            .Optional(TEXT("confirm"), TEXT("boolean"), TEXT("Required true when dry_run=false."), TEXT("false"))
            .Optional(TEXT("compile"), TEXT("boolean"), TEXT("Compile the Widget Blueprint after an applied mutation (default true)."), TEXT("true"))
            .Optional(TEXT("read_back"), TEXT("boolean"), TEXT("Include post-plan/post-write animation overview read-back (default true)."), TEXT("true"))
            .Build()
    );

    Registry.RegisterAction(
        TEXT("ui"), TEXT("remove_animation_binding"),
        TEXT("Confirm-gated removal of one stale UWidgetAnimation binding and its exact MovieScene possessable, tracks, sections, and keys. The widget must be absent from the WidgetTree by default. Slot-widget bindings, ambiguous binding identities, and orphaned possessables fail closed. Dry-run is the default; writes require dry_run=false, confirm=true, and confirm_delete=true."),
        FMonolithActionHandler::CreateStatic(&HandleRemoveAnimationBinding),
        FParamSchemaBuilder()
            .RequiredAssetPath(TEXT("asset_path"), TEXT("Widget Blueprint asset path"))
            .Required(TEXT("animation_name"), TEXT("string"), TEXT("Name/display label of the existing UWidgetAnimation"))
            .Required(TEXT("widget_name"), TEXT("string"), TEXT("Exact stale widget name stored in the animation binding"))
            .Optional(TEXT("dry_run"), TEXT("boolean"), TEXT("Plan only; default true. Set false with both confirmation flags to mutate."), TEXT("true"))
            .Optional(TEXT("confirm"), TEXT("boolean"), TEXT("Required true when dry_run=false."), TEXT("false"))
            .Optional(TEXT("confirm_delete"), TEXT("boolean"), TEXT("Required true when dry_run=false because the MovieScene tracks and keys are deleted."), TEXT("false"))
            .Optional(TEXT("require_widget_missing"), TEXT("boolean"), TEXT("Reject a binding whose widget is still resident in the WidgetTree (default true)."), TEXT("true"))
            .Optional(TEXT("compile"), TEXT("boolean"), TEXT("Compile the Widget Blueprint after an applied mutation (default true)."), TEXT("true"))
            .Optional(TEXT("read_back"), TEXT("boolean"), TEXT("Include post-plan/post-write animation overview read-back (default true)."), TEXT("true"))
            .Build()
    );

    Registry.RegisterAction(
        TEXT("ui"), TEXT("create_animation"),
        TEXT("[DEPRECATED -- use ui::create_animation_v2 instead] Create a new UWidgetAnimation with tracks and keyframes on a Widget Blueprint. Scheduled for removal one major release out (Phase L marker, 2026-04-26). Response payload is tagged {deprecated: true, use_action: \"ui::create_animation_v2\"}; the v2 surface supports multi-track + cubic / weighted-tangent interpolation that v1 cannot express."),
        FMonolithActionHandler::CreateStatic(&HandleCreateAnimation),
        FParamSchemaBuilder()
            .RequiredAssetPath(TEXT("asset_path"), TEXT("Widget Blueprint asset path"))
            .Required(TEXT("animation_name"), TEXT("string"), TEXT("Name for the new animation"))
            .Required(TEXT("duration"), TEXT("number"), TEXT("Animation duration in seconds"))
            .Optional(TEXT("tracks"), TEXT("array"), TEXT("Array of track definitions: [{\"widget_name\": \"MyWidget\", \"property\": \"opacity|transform|color\", \"keyframes\": [{\"time\": 0.0, \"value\": 1.0}]}]"))
            .Build()
    );

    Registry.RegisterAction(
        TEXT("ui"), TEXT("add_animation_keyframe"),
        TEXT("[DEPRECATED -- use ui::create_animation_v2 (multi-key authoring) or ui::add_bezier_eased_segment instead] Add a single keyframe to an existing animation track. Scheduled for removal one major release out (Phase L marker, 2026-04-26). Response payload is tagged {deprecated: true, use_action: \"ui::create_animation_v2\"}; the v2 surface authors entire tracks at once and supports the cubic / weighted-tangent shapes that v1 cannot express."),
        FMonolithActionHandler::CreateStatic(&HandleAddAnimationKeyframe),
        FParamSchemaBuilder()
            .RequiredAssetPath(TEXT("asset_path"), TEXT("Widget Blueprint asset path"))
            .Required(TEXT("animation_name"), TEXT("string"), TEXT("Name of the UWidgetAnimation"))
            .Required(TEXT("widget_name"), TEXT("string"), TEXT("Target widget name"))
            .Required(TEXT("property"), TEXT("string"), TEXT("Property: opacity, transform, color"))
            .Optional(TEXT("component"), TEXT("string"), TEXT("For transform: tx, ty, angle, sx, sy. For color: r, g, b, a"))
            .Required(TEXT("time"), TEXT("number"), TEXT("Keyframe time in seconds"))
            .Required(TEXT("value"), TEXT("number"), TEXT("Keyframe value for the selected property/component"))
            .Build()
    );

    Registry.RegisterAction(
        TEXT("ui"), TEXT("remove_animation"),
        TEXT("Remove a UWidgetAnimation from a Widget Blueprint"),
        FMonolithActionHandler::CreateStatic(&HandleRemoveAnimation),
        FParamSchemaBuilder()
            .RequiredAssetPath(TEXT("asset_path"), TEXT("Widget Blueprint asset path"))
            .Required(TEXT("animation_name"), TEXT("string"), TEXT("Name of the animation to remove"))
            .Build()
    );

    FMonolithToolRegistry::Get().SetActionSearchMetadata(TEXT("ui"), TEXT("get_animation_overview"),
        { TEXT("UMG animation overview"), TEXT("WidgetAnimation bindings"), TEXT("MovieScene tracks"), TEXT("animation key counts") },
        { TEXT("animation_overview"), TEXT("get_all_animations"), TEXT("get_animation_full_data"), TEXT("get_animated_widgets") },
        { TEXT("summarize bindings, tracks, key counts, and event rows for ReadFade"), TEXT("list all animations and their MovieScene bindings for a WBP") });
    FMonolithToolRegistry::Get().SetActionSearchMetadata(TEXT("ui"), TEXT("get_animation_timeline"),
        { TEXT("UMG animation timeline"), TEXT("animation keyframes"), TEXT("MovieScene float keys"), TEXT("event track keys") },
        { TEXT("animation_widget_properties"), TEXT("get_animation_keyframes"), TEXT("get_widget_animation_data") },
        { TEXT("show sorted RenderOpacity keys and event keys for a widget animation"), TEXT("inspect animation timeline rows for MyImage") });
    FMonolithToolRegistry::Get().SetActionSearchMetadata(TEXT("ui"), TEXT("get_animation_time_slice"),
        { TEXT("UMG animation sample"), TEXT("time slice"), TEXT("evaluate RenderOpacity"), TEXT("exact event match") },
        { TEXT("animation_time_properties"), TEXT("sample_widget_animation"), TEXT("animation_sample") },
        { TEXT("sample RenderOpacity at t=0.25"), TEXT("show animation values and exact event matches at multiple times") });
    FMonolithToolRegistry::Get().SetActionSearchMetadata(TEXT("ui"), TEXT("apply_animation_delta"),
        { TEXT("UMG animation delta"), TEXT("MovieScene float key merge"), TEXT("animation key delete"), TEXT("confirm-gated animation edit"), TEXT("RenderTransform component key"), TEXT("ColorAndOpacity component key") },
        { TEXT("animation_append_widget_tracks"), TEXT("animation_append_time_slice"), TEXT("animation_delete_widget_keys"), TEXT("set_property_keys") },
        { TEXT("add or update a RenderOpacity key without resetting existing keys"), TEXT("patch RenderTransform.Translation.X with property=transform component=tx"), TEXT("delete one exact-frame ColorAndOpacity.A key with confirm_delete=true") });
    FMonolithToolRegistry::Get().SetActionSearchMetadata(TEXT("ui"), TEXT("remap_animation_binding"),
        { TEXT("UMG animation binding remap"), TEXT("replace animated widget"), TEXT("repair missing animation widget"), TEXT("preserve MovieScene binding GUID") },
        { TEXT("replace_animation_widget_binding"), TEXT("rename_animation_binding_target"), TEXT("repair_stale_widget_animation_binding") },
        { TEXT("remap a stale AnimBoundBotsBorder binding to TagChaseBotSetup without recreating OnActivated"), TEXT("replace an animation widget target while preserving tracks and keys") });
    FMonolithToolRegistry::Get().SetActionSearchMetadata(TEXT("ui"), TEXT("remove_animation_binding"),
        { TEXT("UMG stale animation binding removal"), TEXT("delete missing widget animation track"), TEXT("remove orphaned widget animation binding"), TEXT("confirm-gated MovieScene possessable delete") },
        { TEXT("delete_animation_widget_binding"), TEXT("remove_stale_widget_animation_binding"), TEXT("delete_missing_widget_track") },
        { TEXT("remove a stale QuickplayButton binding after that widget was retired"), TEXT("delete one absent widget binding and all of its MovieScene tracks with confirm_delete=true") });
}

// --- list_animations ---
FMonolithActionResult FMonolithUIAnimationActions::HandleListAnimations(const TSharedPtr<FJsonObject>& Params)
{
    FMonolithActionResult Err;
    FString AssetPath;
    if (!MonolithUIInternal::TryGetRequiredString(Params, TEXT("asset_path"), AssetPath, Err))
    {
        return Err;
    }
    UWidgetBlueprint* WBP = MonolithUIInternal::LoadWidgetBlueprint(AssetPath, Err);
    if (!WBP) return Err;

    TArray<TSharedPtr<FJsonValue>> AnimArray;
    AnimArray.Reserve(WBP->Animations.Num());

    for (UWidgetAnimation* Anim : WBP->Animations)
    {
        if (!Anim) continue;

        TSharedPtr<FJsonObject> AnimObj = MakeShared<FJsonObject>();
        AnimObj->SetStringField(TEXT("name"), Anim->GetName());

        UMovieScene* MovieScene = Anim->GetMovieScene();
        if (MovieScene)
        {
            const UMovieScene* ConstMovieScene = MovieScene;
            FFrameRate TickRes = MovieScene->GetTickResolution();
            TRange<FFrameNumber> PlayRange = MovieScene->GetPlaybackRange();

            double StartTime = PlayRange.GetLowerBoundValue().Value / TickRes.AsDecimal();
            double EndTime = PlayRange.GetUpperBoundValue().Value / TickRes.AsDecimal();

            AnimObj->SetNumberField(TEXT("start_time"), StartTime);
            AnimObj->SetNumberField(TEXT("end_time"), EndTime);
            AnimObj->SetNumberField(TEXT("binding_count"), ConstMovieScene->GetBindings().Num());
        }

        AnimArray.Add(MakeShared<FJsonValueObject>(AnimObj));
    }

    TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
    Result->SetStringField(TEXT("asset_path"), AssetPath);
    Result->SetArrayField(TEXT("animations"), AnimArray);
    Result->SetNumberField(TEXT("count"), AnimArray.Num());
    return FMonolithActionResult::Success(Result);
}

// --- remap_animation_binding ---
FMonolithActionResult FMonolithUIAnimationActions::HandleRemapAnimationBinding(
    const TSharedPtr<FJsonObject>& Params)
{
    FMonolithActionResult Err;
    FString AssetPath;
    if (!MonolithUIInternal::TryGetRequiredString(
            Params, TEXT("asset_path"), AssetPath, Err))
    {
        return Err;
    }

    FString AnimationName;
    if (!MonolithUIInternal::TryGetRequiredString(
            Params, TEXT("animation_name"), AnimationName, Err))
    {
        return Err;
    }

    FString FromWidgetName;
    if (!MonolithUIInternal::TryGetRequiredString(
            Params, TEXT("from_widget_name"), FromWidgetName, Err))
    {
        return Err;
    }

    FString ToWidgetName;
    if (!MonolithUIInternal::TryGetRequiredString(
            Params, TEXT("to_widget_name"), ToWidgetName, Err))
    {
        return Err;
    }

    if (FromWidgetName == ToWidgetName)
    {
        return FMonolithActionResult::Error(
            TEXT("from_widget_name and to_widget_name must be different"),
            -32602);
    }

    const bool bDryRun =
        MonolithUIInternal::GetOptionalBool(Params, TEXT("dry_run"), true);
    const bool bConfirm =
        MonolithUIInternal::GetOptionalBool(Params, TEXT("confirm"), false);
    const bool bCompile =
        MonolithUIInternal::GetOptionalBool(Params, TEXT("compile"), true);
    const bool bReadBack =
        MonolithUIInternal::GetOptionalBool(Params, TEXT("read_back"), true);

    if (!bDryRun && !bConfirm)
    {
        return FMonolithActionResult::Error(
            TEXT("remap_animation_binding writes require dry_run=false and confirm=true"),
            -32602);
    }

    UWidgetBlueprint* WBP =
        MonolithUIInternal::LoadWidgetBlueprint(AssetPath, Err);
    if (!WBP)
    {
        return Err;
    }

    UWidgetAnimation* Animation =
        FindAnimationForRead(WBP, AnimationName);
    if (!Animation)
    {
        return FMonolithActionResult::Error(
            FString::Printf(
                TEXT("Animation '%s' not found on '%s'"),
                *AnimationName,
                *AssetPath),
            -32603);
    }

    UMovieScene* MovieScene = Animation->GetMovieScene();
    if (!MovieScene)
    {
        return FMonolithActionResult::Error(
            TEXT("Animation has no MovieScene"),
            -32603);
    }

    if (!WBP->WidgetTree)
    {
        return FMonolithActionResult::Error(
            TEXT("Widget Blueprint has no WidgetTree"),
            -32603);
    }

    UWidget* TargetWidget =
        WBP->WidgetTree->FindWidget(FName(*ToWidgetName));
    if (!TargetWidget)
    {
        return FMonolithActionResult::Error(
            FString::Printf(
                TEXT("to_widget_name '%s' is not resident in '%s'"),
                *ToWidgetName,
                *AssetPath),
            -32602);
    }

    const FName FromName(*FromWidgetName);
    const FName ToName(*ToWidgetName);
    FWidgetAnimationBinding* SourceBinding = nullptr;
    FWidgetAnimationBinding* ExistingTargetBinding = nullptr;
    int32 SourceBindingCount = 0;
    int32 TargetBindingCount = 0;

    for (FWidgetAnimationBinding& Binding : Animation->AnimationBindings)
    {
        if (Binding.WidgetName == FromName)
        {
            SourceBinding = &Binding;
            ++SourceBindingCount;
        }
        if (Binding.WidgetName == ToName)
        {
            ExistingTargetBinding = &Binding;
            ++TargetBindingCount;
        }
    }

    if (SourceBindingCount > 1 || TargetBindingCount > 1)
    {
        return FMonolithActionResult::Error(
            FString::Printf(
                TEXT("Animation binding names must be unique: from=%s count=%d to=%s count=%d"),
                *FromWidgetName,
                SourceBindingCount,
                *ToWidgetName,
                TargetBindingCount),
            -32603);
    }

    if (SourceBinding && ExistingTargetBinding)
    {
        return FMonolithActionResult::Error(
            FString::Printf(
                TEXT("Animation already contains distinct bindings for both '%s' and '%s'; binding merge is not supported"),
                *FromWidgetName,
                *ToWidgetName),
            -32602);
    }

    FWidgetAnimationBinding* BindingToRemap =
        SourceBinding ? SourceBinding : ExistingTargetBinding;
    if (!BindingToRemap)
    {
        return FMonolithActionResult::Error(
            FString::Printf(
                TEXT("Animation '%s' contains no binding for '%s' or already-remapped target '%s'"),
                *AnimationName,
                *FromWidgetName,
                *ToWidgetName),
            -32603);
    }

    if (!BindingToRemap->SlotWidgetName.IsNone())
    {
        return FMonolithActionResult::Error(
            FString::Printf(
                TEXT("Animation binding '%s' targets slot widget '%s'; slot-widget remapping is not supported"),
                *BindingToRemap->WidgetName.ToString(),
                *BindingToRemap->SlotWidgetName.ToString()),
            -32602);
    }

    FMovieScenePossessable* Possessable =
        MovieScene->FindPossessable(BindingToRemap->AnimationGuid);
    if (!Possessable)
    {
        return FMonolithActionResult::Error(
            FString::Printf(
                TEXT("Animation binding '%s' has no possessable for GUID %s"),
                *BindingToRemap->WidgetName.ToString(),
                *BindingToRemap->AnimationGuid.ToString(
                    EGuidFormats::DigitsWithHyphensLower)),
            -32603);
    }

    const bool bTargetIsRootWidget =
        WBP->WidgetTree->RootWidget == TargetWidget;
    bool bPossessedClassDiffers = false;
#if WITH_EDITORONLY_DATA
    bPossessedClassDiffers =
        Possessable->GetLoadedPossessedObjectClass() != TargetWidget->GetClass();
#endif
    const bool bWouldChange =
        BindingToRemap->WidgetName != ToName
        || BindingToRemap->bIsRootWidget != bTargetIsRootWidget
        || Possessable->GetName() != ToWidgetName
        || bPossessedClassDiffers;
    const bool bAlreadyRemapped =
        SourceBinding == nullptr && ExistingTargetBinding != nullptr;

    bool bMutated = false;
    bool bCompiled = false;
    if (!bDryRun && bWouldChange)
    {
        WBP->Modify();
        Animation->Modify();
        MovieScene->Modify();

        BindingToRemap->WidgetName = ToName;
        BindingToRemap->bIsRootWidget = bTargetIsRootWidget;
        Possessable->SetName(ToWidgetName);
#if WITH_EDITORONLY_DATA
        Possessable->SetPossessedObjectClass(TargetWidget->GetClass());
#endif

        FBlueprintEditorUtils::MarkBlueprintAsModified(WBP);
        WBP->MarkPackageDirty();
        bMutated = true;
        if (bCompile)
        {
            FKismetEditorUtilities::CompileBlueprint(WBP);
            bCompiled = true;
        }
    }

    TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
    Result->SetStringField(
        TEXT("schema_version"),
        TEXT("ui_animation_binding_remap.v1"));
    Result->SetStringField(TEXT("asset_path"), AssetPath);
    Result->SetStringField(
        TEXT("animation_name"),
        GetAnimationReadableName(Animation));
    Result->SetStringField(
        TEXT("owner_action"),
        TEXT("ui.remap_animation_binding"));
    Result->SetStringField(TEXT("from_widget_name"), FromWidgetName);
    Result->SetStringField(TEXT("to_widget_name"), ToWidgetName);
    Result->SetStringField(
        TEXT("binding_guid"),
        BindingToRemap->AnimationGuid.ToString(
            EGuidFormats::DigitsWithHyphensLower));
    Result->SetBoolField(TEXT("dry_run"), bDryRun);
    Result->SetBoolField(TEXT("confirmed"), bConfirm);
    Result->SetBoolField(TEXT("source_binding_found"), SourceBinding != nullptr);
    Result->SetBoolField(
        TEXT("target_binding_found"),
        ExistingTargetBinding != nullptr);
    Result->SetBoolField(TEXT("already_remapped"), bAlreadyRemapped);
    Result->SetBoolField(TEXT("would_change"), bWouldChange);
    Result->SetBoolField(TEXT("mutated"), bMutated);
    Result->SetBoolField(TEXT("compiled"), bCompiled);
    Result->SetBoolField(
        TEXT("compile_succeeded"),
        !bCompiled || WBP->Status != BS_Error);
    if (bReadBack)
    {
        Result->SetObjectField(
            TEXT("read_back_overview"),
            MakeAnimationOverview(WBP, Animation));
    }
    return FMonolithActionResult::Success(Result);
}

// --- remove_animation_binding ---
FMonolithActionResult FMonolithUIAnimationActions::HandleRemoveAnimationBinding(
    const TSharedPtr<FJsonObject>& Params)
{
    FMonolithActionResult Err;
    FString AssetPath;
    if (!MonolithUIInternal::TryGetRequiredString(
            Params, TEXT("asset_path"), AssetPath, Err))
    {
        return Err;
    }

    FString AnimationName;
    if (!MonolithUIInternal::TryGetRequiredString(
            Params, TEXT("animation_name"), AnimationName, Err))
    {
        return Err;
    }

    FString WidgetName;
    if (!MonolithUIInternal::TryGetRequiredString(
            Params, TEXT("widget_name"), WidgetName, Err))
    {
        return Err;
    }

    const bool bDryRun =
        MonolithUIInternal::GetOptionalBool(Params, TEXT("dry_run"), true);
    const bool bConfirm =
        MonolithUIInternal::GetOptionalBool(Params, TEXT("confirm"), false);
    const bool bConfirmDelete =
        MonolithUIInternal::GetOptionalBool(
            Params, TEXT("confirm_delete"), false);
    const bool bRequireWidgetMissing =
        MonolithUIInternal::GetOptionalBool(
            Params, TEXT("require_widget_missing"), true);
    const bool bCompile =
        MonolithUIInternal::GetOptionalBool(Params, TEXT("compile"), true);
    const bool bReadBack =
        MonolithUIInternal::GetOptionalBool(
            Params, TEXT("read_back"), true);

    if (!bDryRun && (!bConfirm || !bConfirmDelete))
    {
        return FMonolithActionResult::Error(
            TEXT("remove_animation_binding writes require dry_run=false, confirm=true, and confirm_delete=true"),
            -32602);
    }

    UWidgetBlueprint* WBP =
        MonolithUIInternal::LoadWidgetBlueprint(AssetPath, Err);
    if (!WBP)
    {
        return Err;
    }

    UWidgetAnimation* Animation =
        FindAnimationForRead(WBP, AnimationName);
    if (!Animation)
    {
        return FMonolithActionResult::Error(
            FString::Printf(
                TEXT("Animation '%s' not found on '%s'"),
                *AnimationName,
                *AssetPath),
            -32603);
    }

    UMovieScene* MovieScene = Animation->GetMovieScene();
    if (!MovieScene)
    {
        return FMonolithActionResult::Error(
            TEXT("Animation has no MovieScene"),
            -32603);
    }

    if (!WBP->WidgetTree)
    {
        return FMonolithActionResult::Error(
            TEXT("Widget Blueprint has no WidgetTree"),
            -32603);
    }

    const FName WidgetFName(*WidgetName);
    UWidget* ResidentWidget = WBP->WidgetTree->FindWidget(WidgetFName);
    if (bRequireWidgetMissing && ResidentWidget)
    {
        return FMonolithActionResult::Error(
            FString::Printf(
                TEXT("widget_name '%s' is still resident in '%s'; remove it from the WidgetTree first or explicitly set require_widget_missing=false"),
                *WidgetName,
                *AssetPath),
            -32602);
    }

    int32 MatchingBindingIndex = INDEX_NONE;
    int32 MatchingBindingCount = 0;
    for (int32 Index = 0; Index < Animation->AnimationBindings.Num(); ++Index)
    {
        if (Animation->AnimationBindings[Index].WidgetName == WidgetFName)
        {
            MatchingBindingIndex = Index;
            ++MatchingBindingCount;
        }
    }

    if (MatchingBindingCount > 1)
    {
        return FMonolithActionResult::Error(
            FString::Printf(
                TEXT("Animation '%s' contains %d bindings for widget '%s'; removal requires one exact binding"),
                *AnimationName,
                MatchingBindingCount,
                *WidgetName),
            -32603);
    }

    int32 NamedPossessableCount = 0;
    for (int32 Index = 0; Index < MovieScene->GetPossessableCount(); ++Index)
    {
        if (MovieScene->GetPossessable(Index).GetName() == WidgetName)
        {
            ++NamedPossessableCount;
        }
    }

    if (MatchingBindingCount == 0 && NamedPossessableCount > 0)
    {
        return FMonolithActionResult::Error(
            FString::Printf(
                TEXT("Animation '%s' has no FWidgetAnimationBinding for '%s' but retains %d named MovieScene possessable(s); orphan repair is ambiguous"),
                *AnimationName,
                *WidgetName,
                NamedPossessableCount),
            -32603);
    }

    FGuid BindingGuid;
    int32 TrackCount = 0;
    bool bBindingFound = MatchingBindingCount == 1;
    if (bBindingFound)
    {
        const FWidgetAnimationBinding& Binding =
            Animation->AnimationBindings[MatchingBindingIndex];
        if (!Binding.SlotWidgetName.IsNone())
        {
            return FMonolithActionResult::Error(
                FString::Printf(
                    TEXT("Animation binding '%s' targets slot widget '%s'; slot-widget removal is not supported"),
                    *Binding.WidgetName.ToString(),
                    *Binding.SlotWidgetName.ToString()),
                -32602);
        }

        BindingGuid = Binding.AnimationGuid;
        int32 GuidBindingCount = 0;
        for (const FWidgetAnimationBinding& Candidate :
             Animation->AnimationBindings)
        {
            GuidBindingCount +=
                Candidate.AnimationGuid == BindingGuid ? 1 : 0;
        }
        if (GuidBindingCount != 1)
        {
            return FMonolithActionResult::Error(
                FString::Printf(
                    TEXT("Animation binding GUID %s is referenced %d times; removal requires one exact binding identity"),
                    *BindingGuid.ToString(
                        EGuidFormats::DigitsWithHyphensLower),
                    GuidBindingCount),
                -32603);
        }

        FMovieScenePossessable* Possessable =
            MovieScene->FindPossessable(BindingGuid);
        if (!Possessable)
        {
            return FMonolithActionResult::Error(
                FString::Printf(
                    TEXT("Animation binding '%s' has no possessable for GUID %s"),
                    *WidgetName,
                    *BindingGuid.ToString(
                        EGuidFormats::DigitsWithHyphensLower)),
                -32603);
        }
        if (Possessable->GetName() != WidgetName
            || NamedPossessableCount != 1)
        {
            return FMonolithActionResult::Error(
                FString::Printf(
                    TEXT("Animation binding '%s' does not resolve to one exact same-named MovieScene possessable (bound_name='%s', named_count=%d)"),
                    *WidgetName,
                    *Possessable->GetName(),
                    NamedPossessableCount),
                -32603);
        }

        const FMovieSceneBinding* MovieSceneBinding =
            static_cast<const UMovieScene*>(MovieScene)->FindBinding(
                BindingGuid);
        if (!MovieSceneBinding)
        {
            return FMonolithActionResult::Error(
                FString::Printf(
                    TEXT("Animation binding '%s' has no MovieScene object binding for GUID %s"),
                    *WidgetName,
                    *BindingGuid.ToString(
                        EGuidFormats::DigitsWithHyphensLower)),
                -32603);
        }
        TrackCount = MovieSceneBinding->GetTracks().Num();
    }

    const bool bAlreadyRemoved = !bBindingFound;
    const bool bWouldChange = bBindingFound;
    bool bMutated = false;
    bool bCompiled = false;
    if (!bDryRun && bWouldChange)
    {
        WBP->Modify();
        Animation->Modify();
        MovieScene->Modify();

        if (!MovieScene->RemovePossessable(BindingGuid))
        {
            return FMonolithActionResult::Error(
                FString::Printf(
                    TEXT("Failed to remove MovieScene possessable %s for widget '%s'"),
                    *BindingGuid.ToString(
                        EGuidFormats::DigitsWithHyphensLower),
                    *WidgetName),
                -32603);
        }
        Animation->UnbindPossessableObjects(BindingGuid);

        FBlueprintEditorUtils::MarkBlueprintAsModified(WBP);
        WBP->MarkPackageDirty();
        bMutated = true;
        if (bCompile)
        {
            FKismetEditorUtilities::CompileBlueprint(WBP);
            bCompiled = true;
        }
    }

    TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
    Result->SetStringField(
        TEXT("schema_version"),
        TEXT("ui_animation_binding_remove.v1"));
    Result->SetStringField(TEXT("asset_path"), AssetPath);
    Result->SetStringField(
        TEXT("animation_name"),
        GetAnimationReadableName(Animation));
    Result->SetStringField(
        TEXT("owner_action"),
        TEXT("ui.remove_animation_binding"));
    Result->SetStringField(TEXT("widget_name"), WidgetName);
    Result->SetStringField(
        TEXT("binding_guid"),
        BindingGuid.IsValid()
            ? BindingGuid.ToString(
                EGuidFormats::DigitsWithHyphensLower)
            : FString());
    Result->SetBoolField(TEXT("dry_run"), bDryRun);
    Result->SetBoolField(TEXT("confirmed"), bConfirm);
    Result->SetBoolField(TEXT("delete_confirmed"), bConfirmDelete);
    Result->SetBoolField(
        TEXT("require_widget_missing"),
        bRequireWidgetMissing);
    Result->SetBoolField(TEXT("widget_resident"), ResidentWidget != nullptr);
    Result->SetBoolField(TEXT("binding_found"), bBindingFound);
    Result->SetNumberField(TEXT("track_count"), TrackCount);
    Result->SetBoolField(TEXT("already_removed"), bAlreadyRemoved);
    Result->SetBoolField(TEXT("would_change"), bWouldChange);
    Result->SetBoolField(TEXT("mutated"), bMutated);
    Result->SetBoolField(TEXT("compiled"), bCompiled);
    Result->SetBoolField(
        TEXT("compile_succeeded"),
        !bCompiled || WBP->Status != BS_Error);
    if (bReadBack)
    {
        Result->SetObjectField(
            TEXT("read_back_overview"),
            MakeAnimationOverview(WBP, Animation));
    }
    return FMonolithActionResult::Success(Result);
}

// --- get_animation_details ---
FMonolithActionResult FMonolithUIAnimationActions::HandleGetAnimationDetails(const TSharedPtr<FJsonObject>& Params)
{
    FMonolithActionResult Err;
    FString AssetPath;
    if (!MonolithUIInternal::TryGetRequiredString(Params, TEXT("asset_path"), AssetPath, Err))
    {
        return Err;
    }
    FString AnimationName;
    if (!MonolithUIInternal::TryGetRequiredString(Params, TEXT("animation_name"), AnimationName, Err))
    {
        return Err;
    }

    UWidgetBlueprint* WBP = MonolithUIInternal::LoadWidgetBlueprint(AssetPath, Err);
    if (!WBP) return Err;

    // Find the animation by name
    UWidgetAnimation* TargetAnim = nullptr;
    for (UWidgetAnimation* Anim : WBP->Animations)
    {
        if (Anim && Anim->GetName() == AnimationName)
        {
            TargetAnim = Anim;
            break;
        }
    }

    if (!TargetAnim)
    {
        return FMonolithActionResult::Error(
            FString::Printf(TEXT("Animation '%s' not found on '%s'"), *AnimationName, *AssetPath));
    }

    UMovieScene* MovieScene = TargetAnim->GetMovieScene();
    if (!MovieScene)
    {
        return FMonolithActionResult::Error(TEXT("Animation has no MovieScene"));
    }

    FFrameRate TickRes = MovieScene->GetTickResolution();

    // Iterate all tracks
    TArray<TSharedPtr<FJsonValue>> TrackArray;
    TrackArray.Reserve(MovieScene->GetTracks().Num());
    for (UMovieSceneTrack* Track : MovieScene->GetTracks())
    {
        if (!Track) continue;

        TSharedPtr<FJsonObject> TrackObj = MakeShared<FJsonObject>();
        TrackObj->SetStringField(TEXT("track_type"), Track->GetClass()->GetName());

        // Get sections
        TArray<TSharedPtr<FJsonValue>> SectionArray;
        SectionArray.Reserve(Track->GetAllSections().Num());
        for (UMovieSceneSection* Section : Track->GetAllSections())
        {
            if (!Section) continue;

            TSharedPtr<FJsonObject> SecObj = MakeShared<FJsonObject>();

            TRange<FFrameNumber> SectionRange = Section->GetRange();
            if (SectionRange.HasLowerBound())
            {
                SecObj->SetNumberField(TEXT("start_time"),
                    SectionRange.GetLowerBoundValue().Value / TickRes.AsDecimal());
            }
            if (SectionRange.HasUpperBound())
            {
                SecObj->SetNumberField(TEXT("end_time"),
                    SectionRange.GetUpperBoundValue().Value / TickRes.AsDecimal());
            }

            // Count keyframes across all channels
            int32 TotalKeys = 0;
            FMovieSceneChannelProxy& ChannelProxy = Section->GetChannelProxy();
            for (const FMovieSceneChannelEntry& Entry : ChannelProxy.GetAllEntries())
            {
                for (FMovieSceneChannel* Channel : Entry.GetChannels())
                {
                    if (Channel)
                    {
                        TotalKeys += Channel->GetNumKeys();
                    }
                }
            }
            SecObj->SetNumberField(TEXT("keyframe_count"), TotalKeys);

            SectionArray.Add(MakeShared<FJsonValueObject>(SecObj));
        }
        TrackObj->SetArrayField(TEXT("sections"), SectionArray);

        TrackArray.Add(MakeShared<FJsonValueObject>(TrackObj));
    }

    // Also iterate bound object tracks
    const UMovieScene* ConstMovieScene = MovieScene;
    for (const FMovieSceneBinding& Binding : ConstMovieScene->GetBindings())
    {
        for (UMovieSceneTrack* Track : Binding.GetTracks())
        {
            if (!Track) continue;

            TSharedPtr<FJsonObject> TrackObj = MakeShared<FJsonObject>();
            TrackObj->SetStringField(TEXT("binding_name"), ResolveBindingName(MovieScene, Binding));
            TrackObj->SetStringField(TEXT("track_type"), Track->GetClass()->GetName());

            TArray<TSharedPtr<FJsonValue>> SectionArray;
            SectionArray.Reserve(Track->GetAllSections().Num());
            for (UMovieSceneSection* Section : Track->GetAllSections())
            {
                if (!Section) continue;

                TSharedPtr<FJsonObject> SecObj = MakeShared<FJsonObject>();
                TRange<FFrameNumber> SectionRange = Section->GetRange();
                if (SectionRange.HasLowerBound())
                {
                    SecObj->SetNumberField(TEXT("start_time"),
                        SectionRange.GetLowerBoundValue().Value / TickRes.AsDecimal());
                }
                if (SectionRange.HasUpperBound())
                {
                    SecObj->SetNumberField(TEXT("end_time"),
                        SectionRange.GetUpperBoundValue().Value / TickRes.AsDecimal());
                }

                int32 TotalKeys = 0;
                FMovieSceneChannelProxy& ChannelProxy = Section->GetChannelProxy();
                for (const FMovieSceneChannelEntry& Entry : ChannelProxy.GetAllEntries())
                {
                    for (FMovieSceneChannel* Channel : Entry.GetChannels())
                    {
                        if (Channel)
                        {
                            TotalKeys += Channel->GetNumKeys();
                        }
                    }
                }
                SecObj->SetNumberField(TEXT("keyframe_count"), TotalKeys);

                SectionArray.Add(MakeShared<FJsonValueObject>(SecObj));
            }
            TrackObj->SetArrayField(TEXT("sections"), SectionArray);

            TrackArray.Add(MakeShared<FJsonValueObject>(TrackObj));
        }
    }

    TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
    Result->SetStringField(TEXT("asset_path"), AssetPath);
    Result->SetStringField(TEXT("animation_name"), AnimationName);
    Result->SetArrayField(TEXT("tracks"), TrackArray);
    Result->SetNumberField(TEXT("track_count"), TrackArray.Num());
    return FMonolithActionResult::Success(Result);
}

// --- get_animation_overview ---
FMonolithActionResult FMonolithUIAnimationActions::HandleGetAnimationOverview(const TSharedPtr<FJsonObject>& Params)
{
    FMonolithActionResult Err;
    FString AssetPath;
    if (!MonolithUIInternal::TryGetRequiredString(Params, TEXT("asset_path"), AssetPath, Err))
    {
        return Err;
    }

    bool bIncludeAll = false;
    Params->TryGetBoolField(TEXT("include_all"), bIncludeAll);

    FString AnimationName;
    Params->TryGetStringField(TEXT("animation_name"), AnimationName);
    if (!bIncludeAll && AnimationName.IsEmpty())
    {
        return FMonolithActionResult::Error(
            TEXT("animation_name is required unless include_all=true"),
            -32602);
    }

    UWidgetBlueprint* WBP = MonolithUIInternal::LoadWidgetBlueprint(AssetPath, Err);
    if (!WBP)
    {
        return Err;
    }

    TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
    Result->SetStringField(TEXT("schema_version"), TEXT("ui_animation_overview.v1"));
    Result->SetStringField(TEXT("asset_path"), AssetPath);
    Result->SetBoolField(TEXT("read_only"), true);
    Result->SetBoolField(TEXT("include_all"), bIncludeAll);
    Result->SetStringField(TEXT("owner_action"), TEXT("ui.get_animation_overview"));
    TArray<TSharedPtr<FJsonValue>> ExternalAliases;
    ExternalAliases.Reserve(3);
    ExternalAliases.Add(MakeShared<FJsonValueString>(TEXT("animation_overview")));
    ExternalAliases.Add(MakeShared<FJsonValueString>(TEXT("animation_widget_properties")));
    ExternalAliases.Add(MakeShared<FJsonValueString>(TEXT("animation_time_properties")));
    Result->SetArrayField(TEXT("external_aliases_not_registered"), ExternalAliases);

    if (bIncludeAll)
    {
        TArray<TSharedPtr<FJsonValue>> Animations;
        Animations.Reserve(WBP->Animations.Num());
        for (UWidgetAnimation* Animation : WBP->Animations)
        {
            if (!Animation)
            {
                continue;
            }
            Animations.Add(MakeShared<FJsonValueObject>(MakeAnimationOverview(WBP, Animation)));
        }
        Result->SetArrayField(TEXT("animations"), Animations);
        Result->SetNumberField(TEXT("count"), Animations.Num());
        return FMonolithActionResult::Success(Result);
    }

    UWidgetAnimation* Animation = FindAnimationForRead(WBP, AnimationName);
    if (!Animation)
    {
        return FMonolithActionResult::Error(
            FString::Printf(TEXT("Animation '%s' not found on '%s'"), *AnimationName, *AssetPath),
            -32603);
    }

    Result->SetStringField(TEXT("animation_name"), GetAnimationReadableName(Animation));
    Result->SetObjectField(TEXT("animation"), MakeAnimationOverview(WBP, Animation));
    return FMonolithActionResult::Success(Result);
}

// --- get_animation_timeline ---
FMonolithActionResult FMonolithUIAnimationActions::HandleGetAnimationTimeline(const TSharedPtr<FJsonObject>& Params)
{
    FMonolithActionResult Err;
    FString AssetPath;
    if (!MonolithUIInternal::TryGetRequiredString(Params, TEXT("asset_path"), AssetPath, Err))
    {
        return Err;
    }

    FString AnimationName;
    if (!MonolithUIInternal::TryGetRequiredString(Params, TEXT("animation_name"), AnimationName, Err))
    {
        return Err;
    }

    FString WidgetFilter;
    Params->TryGetStringField(TEXT("widget_name"), WidgetFilter);
    FString PropertyFilter;
    Params->TryGetStringField(TEXT("property_path"), PropertyFilter);
    bool bIncludeEvents = true;
    Params->TryGetBoolField(TEXT("include_events"), bIncludeEvents);

    double MaxRowsNumber = 1000.0;
    Params->TryGetNumberField(TEXT("max_rows"), MaxRowsNumber);
    const int32 MaxRows = FMath::Max(0, static_cast<int32>(MaxRowsNumber));

    UWidgetBlueprint* WBP = MonolithUIInternal::LoadWidgetBlueprint(AssetPath, Err);
    if (!WBP)
    {
        return Err;
    }

    UWidgetAnimation* Animation = FindAnimationForRead(WBP, AnimationName);
    if (!Animation)
    {
        return FMonolithActionResult::Error(
            FString::Printf(TEXT("Animation '%s' not found on '%s'"), *AnimationName, *AssetPath),
            -32603);
    }

    UMovieScene* MovieScene = Animation->GetMovieScene();
    if (!MovieScene)
    {
        return FMonolithActionResult::Error(TEXT("Animation has no MovieScene"), -32603);
    }

    TArray<TSharedPtr<FJsonObject>> Rows;
    const UMovieScene* ConstMovieScene = MovieScene;
    for (const FMovieSceneBinding& Binding : ConstMovieScene->GetBindings())
    {
        for (UMovieSceneTrack* Track : Binding.GetTracks())
        {
            if (const UMovieSceneFloatTrack* FloatTrack = Cast<UMovieSceneFloatTrack>(Track))
            {
                AppendFloatTimelineRows(Animation, MovieScene, Binding, FloatTrack, WidgetFilter, PropertyFilter, Rows);
            }
        }
    }

    if (bIncludeEvents)
    {
        for (UMovieSceneTrack* Track : MovieScene->GetTracks())
        {
            if (const UMovieSceneEventTrack* EventTrack = Cast<UMovieSceneEventTrack>(Track))
            {
                AppendEventTimelineRows(MovieScene, EventTrack, Rows);
            }
        }
    }

    SortTimelineRows(Rows);

    TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
    Result->SetStringField(TEXT("schema_version"), TEXT("ui_animation_timeline.v1"));
    Result->SetStringField(TEXT("asset_path"), AssetPath);
    Result->SetStringField(TEXT("animation_name"), GetAnimationReadableName(Animation));
    Result->SetBoolField(TEXT("read_only"), true);
    Result->SetStringField(TEXT("owner_action"), TEXT("ui.get_animation_timeline"));
    Result->SetStringField(TEXT("widget_filter"), WidgetFilter);
    Result->SetStringField(TEXT("property_filter"), PropertyFilter);
    Result->SetBoolField(TEXT("include_events"), bIncludeEvents);
    Result->SetNumberField(TEXT("row_count"), Rows.Num());
    Result->SetBoolField(TEXT("truncated"), MaxRows > 0 && Rows.Num() > MaxRows);
    Result->SetArrayField(TEXT("rows"), RowsToJsonValues(Rows, MaxRows));
    return FMonolithActionResult::Success(Result);
}

// --- get_animation_time_slice ---
FMonolithActionResult FMonolithUIAnimationActions::HandleGetAnimationTimeSlice(const TSharedPtr<FJsonObject>& Params)
{
    FMonolithActionResult Err;
    FString AssetPath;
    if (!MonolithUIInternal::TryGetRequiredString(Params, TEXT("asset_path"), AssetPath, Err))
    {
        return Err;
    }

    FString AnimationName;
    if (!MonolithUIInternal::TryGetRequiredString(Params, TEXT("animation_name"), AnimationName, Err))
    {
        return Err;
    }

    TArray<double> Times;
    double SingleTime = 0.0;
    if (Params->TryGetNumberField(TEXT("time"), SingleTime))
    {
        Times.Add(SingleTime);
    }

    const TArray<TSharedPtr<FJsonValue>>* TimesArray = nullptr;
    if (Params->TryGetArrayField(TEXT("times"), TimesArray) && TimesArray)
    {
        for (int32 Index = 0; Index < TimesArray->Num(); ++Index)
        {
            double TimeValue = 0.0;
            if (!(*TimesArray)[Index].IsValid() || !(*TimesArray)[Index]->TryGetNumber(TimeValue))
            {
                return FMonolithActionResult::Error(
                    FString::Printf(TEXT("times[%d] must be a number"), Index),
                    -32602);
            }
            Times.Add(TimeValue);
        }
    }

    if (Times.Num() == 0)
    {
        return FMonolithActionResult::Error(TEXT("Provide either time or times[]"), -32602);
    }

    FString WidgetFilter;
    Params->TryGetStringField(TEXT("widget_name"), WidgetFilter);
    FString PropertyFilter;
    Params->TryGetStringField(TEXT("property_path"), PropertyFilter);

    double EventToleranceNumber = 0.0;
    Params->TryGetNumberField(TEXT("event_tolerance_frames"), EventToleranceNumber);
    const int32 EventToleranceFrames = FMath::Max(0, static_cast<int32>(EventToleranceNumber));

    UWidgetBlueprint* WBP = MonolithUIInternal::LoadWidgetBlueprint(AssetPath, Err);
    if (!WBP)
    {
        return Err;
    }

    UWidgetAnimation* Animation = FindAnimationForRead(WBP, AnimationName);
    if (!Animation)
    {
        return FMonolithActionResult::Error(
            FString::Printf(TEXT("Animation '%s' not found on '%s'"), *AnimationName, *AssetPath),
            -32603);
    }

    UMovieScene* MovieScene = Animation->GetMovieScene();
    if (!MovieScene)
    {
        return FMonolithActionResult::Error(TEXT("Animation has no MovieScene"), -32603);
    }

    const FFrameRate TickResolution = MovieScene->GetTickResolution();
    TArray<TSharedPtr<FJsonValue>> Samples;
    Samples.Reserve(Times.Num());
    const UMovieScene* ConstMovieScene = MovieScene;

    for (const double TimeSeconds : Times)
    {
        const FFrameTime FrameTime = TickResolution.AsFrameTime(TimeSeconds);
        const FFrameNumber FrameNumber = FrameTime.GetFrame();

        TArray<TSharedPtr<FJsonObject>> Rows;
        for (const FMovieSceneBinding& Binding : ConstMovieScene->GetBindings())
        {
            const FString WidgetName = ResolveWidgetNameForBinding(Animation, MovieScene, Binding.GetObjectGuid());
            if (!WidgetFilter.IsEmpty() && WidgetName != WidgetFilter)
            {
                continue;
            }

            for (UMovieSceneTrack* Track : Binding.GetTracks())
            {
                const UMovieSceneFloatTrack* FloatTrack = Cast<UMovieSceneFloatTrack>(Track);
                if (!FloatTrack)
                {
                    continue;
                }

                const FString PropertyPath = FloatTrack->GetPropertyPath().ToString();
                if (!PropertyFilter.IsEmpty() && PropertyPath != PropertyFilter && FloatTrack->GetPropertyName().ToString() != PropertyFilter)
                {
                    continue;
                }

                int32 SectionIndex = 0;
                for (UMovieSceneSection* Section : FloatTrack->GetAllSections())
                {
                    const UMovieSceneFloatSection* FloatSection = Cast<UMovieSceneFloatSection>(Section);
                    if (!FloatSection || !IsFrameInSection(Section, FrameNumber))
                    {
                        ++SectionIndex;
                        continue;
                    }

                    float Value = 0.0f;
                    if (FloatSection->GetChannel().Evaluate(FrameTime, Value))
                    {
                        TSharedPtr<FJsonObject> Row = MakeShared<FJsonObject>();
                        Row->SetStringField(TEXT("row_type"), TEXT("property_sample"));
                        Row->SetStringField(TEXT("value_type"), TEXT("float"));
                        Row->SetNumberField(TEXT("frame"), FrameNumber.Value);
                        Row->SetNumberField(TEXT("time"), TimeSeconds);
                        Row->SetStringField(TEXT("binding_guid"), GuidToString(Binding.GetObjectGuid()));
                        Row->SetStringField(TEXT("widget_name"), WidgetName);
                        Row->SetStringField(TEXT("track_class"), FloatTrack->GetClass()->GetName());
                        Row->SetStringField(TEXT("property_name"), FloatTrack->GetPropertyName().ToString());
                        Row->SetStringField(TEXT("property_path"), PropertyPath);
                        Row->SetStringField(TEXT("channel_name"), TEXT("float"));
                        Row->SetNumberField(TEXT("section_index"), SectionIndex);
                        Row->SetNumberField(TEXT("value"), Value);
                        Rows.Add(Row);
                    }
                    ++SectionIndex;
                }
            }
        }

        for (UMovieSceneTrack* Track : MovieScene->GetTracks())
        {
            const UMovieSceneEventTrack* EventTrack = Cast<UMovieSceneEventTrack>(Track);
            if (!EventTrack)
            {
                continue;
            }

            int32 SectionIndex = 0;
            for (UMovieSceneSection* Section : EventTrack->GetAllSections())
            {
                const UMovieSceneEventTriggerSection* TriggerSection = Cast<UMovieSceneEventTriggerSection>(Section);
                if (!TriggerSection)
                {
                    ++SectionIndex;
                    continue;
                }

                const auto EventData = TriggerSection->EventChannel.GetData();
                const TArrayView<const FFrameNumber> EventTimes = EventData.GetTimes();
                const TArrayView<const FMovieSceneEvent> EventValues = EventData.GetValues();
                const int32 NumEvents = FMath::Min(EventTimes.Num(), EventValues.Num());
                for (int32 KeyIndex = 0; KeyIndex < NumEvents; ++KeyIndex)
                {
                    const int32 DeltaFrames = FMath::Abs(EventTimes[KeyIndex].Value - FrameNumber.Value);
                    if (DeltaFrames > EventToleranceFrames)
                    {
                        continue;
                    }

                    TSharedPtr<FJsonObject> Row = MakeShared<FJsonObject>();
                    Row->SetStringField(TEXT("row_type"), TEXT("event_match"));
                    Row->SetStringField(TEXT("value_type"), TEXT("event"));
                    Row->SetNumberField(TEXT("frame"), EventTimes[KeyIndex].Value);
                    Row->SetNumberField(TEXT("time"), FrameToSeconds(EventTimes[KeyIndex], TickResolution));
                    Row->SetNumberField(TEXT("requested_frame"), FrameNumber.Value);
                    Row->SetNumberField(TEXT("requested_time"), TimeSeconds);
                    Row->SetNumberField(TEXT("delta_frames"), DeltaFrames);
                    Row->SetStringField(TEXT("track_class"), EventTrack->GetClass()->GetName());
                    Row->SetStringField(TEXT("event_name"), EventValues[KeyIndex].CompiledFunctionName.ToString());
                    Row->SetNumberField(TEXT("section_index"), SectionIndex);
                    Row->SetNumberField(TEXT("key_index"), KeyIndex);
                    Rows.Add(Row);
                }
                ++SectionIndex;
            }
        }

        SortTimelineRows(Rows);

        TSharedPtr<FJsonObject> Sample = MakeShared<FJsonObject>();
        Sample->SetNumberField(TEXT("time"), TimeSeconds);
        Sample->SetNumberField(TEXT("frame"), FrameNumber.Value);
        Sample->SetNumberField(TEXT("row_count"), Rows.Num());
        Sample->SetArrayField(TEXT("rows"), RowsToJsonValues(Rows, 0));
        Samples.Add(MakeShared<FJsonValueObject>(Sample));
    }

    TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
    Result->SetStringField(TEXT("schema_version"), TEXT("ui_animation_time_slice.v1"));
    Result->SetStringField(TEXT("asset_path"), AssetPath);
    Result->SetStringField(TEXT("animation_name"), GetAnimationReadableName(Animation));
    Result->SetBoolField(TEXT("read_only"), true);
    Result->SetStringField(TEXT("owner_action"), TEXT("ui.get_animation_time_slice"));
    Result->SetStringField(TEXT("widget_filter"), WidgetFilter);
    Result->SetStringField(TEXT("property_filter"), PropertyFilter);
    Result->SetNumberField(TEXT("event_tolerance_frames"), EventToleranceFrames);
    Result->SetArrayField(TEXT("samples"), Samples);
    Result->SetNumberField(TEXT("sample_count"), Samples.Num());
    return FMonolithActionResult::Success(Result);
}

// --- apply_animation_delta ---
FMonolithActionResult FMonolithUIAnimationActions::HandleApplyAnimationDelta(const TSharedPtr<FJsonObject>& Params)
{
    FMonolithActionResult Err;
    FString AssetPath;
    if (!MonolithUIInternal::TryGetRequiredString(Params, TEXT("asset_path"), AssetPath, Err))
    {
        return Err;
    }

    FString AnimationName;
    if (!MonolithUIInternal::TryGetRequiredString(Params, TEXT("animation_name"), AnimationName, Err))
    {
        return Err;
    }

    const TArray<TSharedPtr<FJsonValue>>* OperationsArray = nullptr;
    if (!Params.IsValid() || !Params->TryGetArrayField(TEXT("operations"), OperationsArray) || !OperationsArray)
    {
        return FMonolithActionResult::Error(TEXT("operations must be an array"), -32602);
    }
    if (OperationsArray->Num() == 0)
    {
        return FMonolithActionResult::Error(TEXT("operations must contain at least one delta operation"), -32602);
    }

    const bool bDryRun = MonolithUIInternal::GetOptionalBool(Params, TEXT("dry_run"), true);
    const bool bConfirm = MonolithUIInternal::GetOptionalBool(Params, TEXT("confirm"), false);
    const bool bConfirmDelete = MonolithUIInternal::GetOptionalBool(Params, TEXT("confirm_delete"), false);
    const bool bCompile = MonolithUIInternal::GetOptionalBool(Params, TEXT("compile"), true);
    const bool bReadBack = MonolithUIInternal::GetOptionalBool(Params, TEXT("read_back"), true);

    if (!bDryRun && !bConfirm)
    {
        return FMonolithActionResult::Error(
            TEXT("apply_animation_delta writes require dry_run=false and confirm=true"),
            -32602);
    }

    UWidgetBlueprint* WBP = MonolithUIInternal::LoadWidgetBlueprint(AssetPath, Err);
    if (!WBP)
    {
        return Err;
    }

    UWidgetAnimation* Animation = FindAnimationForRead(WBP, AnimationName);
    if (!Animation)
    {
        return FMonolithActionResult::Error(
            FString::Printf(TEXT("Animation '%s' not found on '%s'"), *AnimationName, *AssetPath),
            -32603);
    }

    UMovieScene* MovieScene = Animation->GetMovieScene();
    if (!MovieScene)
    {
        return FMonolithActionResult::Error(TEXT("Animation has no MovieScene"), -32603);
    }

    const FFrameRate TickResolution = MovieScene->GetTickResolution();
    TArray<FAnimationDeltaOp> Operations;
    Operations.Reserve(OperationsArray->Num());
    bool bHasDelete = false;

    for (int32 Index = 0; Index < OperationsArray->Num(); ++Index)
    {
        const TSharedPtr<FJsonValue>& OperationValue = (*OperationsArray)[Index];
        const TSharedPtr<FJsonObject>* OperationObj = nullptr;
        if (!OperationValue.IsValid()
            || !OperationValue->TryGetObject(OperationObj)
            || !OperationObj
            || !OperationObj->IsValid())
        {
            return FMonolithActionResult::Error(
                FString::Printf(TEXT("operations[%d] must be an object"), Index),
                -32602);
        }

        FAnimationDeltaOp Operation;
        FMonolithActionResult ParseError;
        if (!TryParseAnimationDeltaOperation(*OperationObj, Index, TickResolution, Operation, ParseError))
        {
            return ParseError;
        }
        bHasDelete |= IsDeleteDeltaOp(Operation);
        Operations.Add(Operation);
    }

    if (!bDryRun && bHasDelete)
    {
        bool bEveryDeleteConfirmed = bConfirmDelete;
        if (!bEveryDeleteConfirmed)
        {
            bEveryDeleteConfirmed = true;
            for (const FAnimationDeltaOp& Operation : Operations)
            {
                if (IsDeleteDeltaOp(Operation) && !Operation.bConfirmDelete)
                {
                    bEveryDeleteConfirmed = false;
                    break;
                }
            }
        }

        if (!bEveryDeleteConfirmed)
        {
            return FMonolithActionResult::Error(
                TEXT("delete operations require confirm_delete=true globally or on every delete operation"),
                -32602);
        }
    }

    for (const FAnimationDeltaOp& Operation : Operations)
    {
        if (Operation.BindingGuid.IsValid())
        {
            if (!static_cast<const UMovieScene*>(MovieScene)->FindBinding(Operation.BindingGuid))
            {
                return FMonolithActionResult::Error(
                    FString::Printf(
                        TEXT("operations[%d].binding_guid '%s' does not exist in animation '%s'"),
                        Operation.OperationIndex,
                        *Operation.BindingGuidText,
                        *GetAnimationReadableName(Animation)),
                    -32602);
            }
            if (Operation.WidgetName.IsEmpty())
            {
                continue;
            }
        }

        if (Operation.WidgetName.IsEmpty())
        {
            continue;
        }

        UWidget* TargetWidget = WBP->WidgetTree
            ? WBP->WidgetTree->FindWidget(FName(*Operation.WidgetName))
            : nullptr;
        if (!TargetWidget && Operation.Kind == EAnimationDeltaOpKind::UpsertFloatKey)
        {
            return FMonolithActionResult::Error(
                FString::Printf(
                    TEXT("operations[%d].widget_name '%s' was not found in '%s'"),
                    Operation.OperationIndex,
                    *Operation.WidgetName,
                    *AssetPath),
                -32602);
        }
    }

    TArray<TSharedPtr<FJsonValue>> OperationRows;
    OperationRows.Reserve(Operations.Num());

    int32 OperationsApplied = 0;
    int32 KeysInserted = 0;
    int32 KeysUpdated = 0;
    int32 KeysDeleted = 0;
    int32 BindingsCreated = 0;
    int32 TracksCreated = 0;
    int32 SectionsCreated = 0;
    int32 NotFoundCount = 0;

    for (const FAnimationDeltaOp& Operation : Operations)
    {
        FGuid BindingGuid = Operation.BindingGuid.IsValid()
            ? Operation.BindingGuid
            : FindExistingBindingGuidForWidget(Animation, MovieScene, Operation.WidgetName);

        const bool bWouldCreateBinding =
            Operation.Kind == EAnimationDeltaOpKind::UpsertFloatKey
            && !BindingGuid.IsValid()
            && !Operation.WidgetName.IsEmpty();

        UMovieSceneFloatTrack* ExistingTrack = BindingGuid.IsValid()
            ? FindFloatTrackByPath(MovieScene, BindingGuid, Operation.PropertyPath)
            : nullptr;
        const bool bWouldCreateTrack = Operation.Kind == EAnimationDeltaOpKind::UpsertFloatKey && !ExistingTrack;

        bool bHadExistingKey = false;
        bool bWouldCreateSection = false;
        int32 ExistingDeleteCount = 0;
        if (ExistingTrack)
        {
            bool bFoundSectionForFrame = false;
            for (UMovieSceneSection* Section : ExistingTrack->GetAllSections())
            {
                UMovieSceneFloatSection* FloatSection = Cast<UMovieSceneFloatSection>(Section);
                if (!FloatSection)
                {
                    continue;
                }

                bFoundSectionForFrame |= IsFrameInSection(FloatSection, Operation.Frame);
                const TArrayView<const FFrameNumber> Times = FloatSection->GetChannel().GetTimes();
                for (const FFrameNumber& Time : Times)
                {
                    if (Time == Operation.Frame)
                    {
                        bHadExistingKey = true;
                        ++ExistingDeleteCount;
                    }
                }
            }
            bWouldCreateSection = Operation.Kind == EAnimationDeltaOpKind::UpsertFloatKey && !bFoundSectionForFrame;
        }
        else
        {
            bWouldCreateSection = Operation.Kind == EAnimationDeltaOpKind::UpsertFloatKey;
        }

        if (bDryRun)
        {
            FString Status;
            if (Operation.Kind == EAnimationDeltaOpKind::UpsertFloatKey)
            {
                Status = bHadExistingKey ? TEXT("would_update") : TEXT("would_insert");
            }
            else
            {
                Status = ExistingDeleteCount > 0 ? TEXT("would_delete") : TEXT("not_found");
                if (ExistingDeleteCount == 0)
                {
                    ++NotFoundCount;
                }
            }

            OperationRows.Add(MakeShared<FJsonValueObject>(MakeDeltaOperationRow(
                Operation,
                Status,
                BindingGuid,
                bHadExistingKey,
                ExistingDeleteCount,
                bWouldCreateBinding,
                bWouldCreateTrack,
                bWouldCreateSection)));
            continue;
        }

        if (Operation.Kind == EAnimationDeltaOpKind::UpsertFloatKey)
        {
            WBP->Modify();
            Animation->Modify();
            MovieScene->Modify();

            if (!BindingGuid.IsValid())
            {
                UWidget* TargetWidget = WBP->WidgetTree
                    ? WBP->WidgetTree->FindWidget(FName(*Operation.WidgetName))
                    : nullptr;
                if (!TargetWidget)
                {
                    return FMonolithActionResult::Error(
                        FString::Printf(TEXT("Widget '%s' disappeared before applying delta"), *Operation.WidgetName),
                        -32603);
                }
                BindingGuid = FindOrCreateWidgetAnimationBinding(WBP, Animation, MovieScene, TargetWidget);
                if (!BindingGuid.IsValid())
                {
                    return FMonolithActionResult::Error(TEXT("Failed to create/find WidgetAnimation binding"), -32603);
                }
                ++BindingsCreated;
            }

            bool bTrackCreated = false;
            bool bSectionCreated = false;
            UMovieSceneFloatTrack* FloatTrack = FindOrCreateDeltaFloatTrack(
                MovieScene,
                BindingGuid,
                Operation.PropertyPath,
                Operation.Frame,
                bTrackCreated,
                bSectionCreated);
            if (!FloatTrack)
            {
                return FMonolithActionResult::Error(
                    FString::Printf(
                        TEXT("Failed to create/find float track for property_path '%s'"),
                        *Operation.PropertyPath),
                    -32603);
            }
            TracksCreated += bTrackCreated ? 1 : 0;
            SectionsCreated += bSectionCreated ? 1 : 0;

            UMovieSceneFloatSection* FloatSection = FindOrCreateFloatSectionForFrame(FloatTrack, Operation.Frame, true);
            if (!FloatSection)
            {
                return FMonolithActionResult::Error(TEXT("Failed to create/find float section"), -32603);
            }

            FloatTrack->Modify();
            FloatSection->Modify();
            bHadExistingKey = FindFloatKeyIndex(FloatSection, Operation.Frame) != INDEX_NONE;
            FloatSection->GetChannel().GetData().UpdateOrAddKey(Operation.Frame, MakeDeltaFloatValue(Operation));
            ExpandSectionRangeToFrame(FloatSection, Operation.Frame);
            ExpandPlaybackRangeToFrame(MovieScene, Operation.Frame);

            ++OperationsApplied;
            if (bHadExistingKey)
            {
                ++KeysUpdated;
            }
            else
            {
                ++KeysInserted;
            }

            OperationRows.Add(MakeShared<FJsonValueObject>(MakeDeltaOperationRow(
                Operation,
                bHadExistingKey ? TEXT("updated") : TEXT("inserted"),
                BindingGuid,
                bHadExistingKey,
                0,
                bWouldCreateBinding,
                bTrackCreated,
                bSectionCreated)));
        }
        else if (Operation.Kind == EAnimationDeltaOpKind::DeleteFloatKey)
        {
            int32 DeletedForOperation = 0;
            if (BindingGuid.IsValid() && ExistingTrack)
            {
                WBP->Modify();
                Animation->Modify();
                MovieScene->Modify();
                ExistingTrack->Modify();

                for (UMovieSceneSection* Section : ExistingTrack->GetAllSections())
                {
                    UMovieSceneFloatSection* FloatSection = Cast<UMovieSceneFloatSection>(Section);
                    if (!FloatSection)
                    {
                        continue;
                    }

                    int32 RemovedFromSection = 0;
                    while (true)
                    {
                        TMovieSceneChannelData<FMovieSceneFloatValue> Data = FloatSection->GetChannel().GetData();
                        const int32 KeyIndex = Data.FindKey(Operation.Frame);
                        if (KeyIndex == INDEX_NONE)
                        {
                            break;
                        }
                        if (RemovedFromSection == 0)
                        {
                            FloatSection->Modify();
                        }
                        Data.RemoveKey(KeyIndex);
                        ++RemovedFromSection;
                        ++DeletedForOperation;
                    }
                }
            }

            if (DeletedForOperation > 0)
            {
                ++OperationsApplied;
                KeysDeleted += DeletedForOperation;
            }
            else
            {
                ++NotFoundCount;
            }

            OperationRows.Add(MakeShared<FJsonValueObject>(MakeDeltaOperationRow(
                Operation,
                DeletedForOperation > 0 ? TEXT("deleted") : TEXT("not_found"),
                BindingGuid,
                DeletedForOperation > 0,
                DeletedForOperation,
                false,
                false,
                false)));
        }
    }

    const bool bMutated = !bDryRun && OperationsApplied > 0;
    bool bCompiled = false;
    if (bMutated)
    {
        FBlueprintEditorUtils::MarkBlueprintAsModified(WBP);
        WBP->MarkPackageDirty();
        if (bCompile)
        {
            FKismetEditorUtilities::CompileBlueprint(WBP);
            bCompiled = true;
        }
    }

    TArray<TSharedPtr<FJsonValue>> SupportedOps;
    SupportedOps.Reserve(2);
    SupportedOps.Add(MakeShared<FJsonValueString>(TEXT("upsert_float_key")));
    SupportedOps.Add(MakeShared<FJsonValueString>(TEXT("delete_float_key")));

    TArray<TSharedPtr<FJsonValue>> SupportedPropertyGroups;
    SupportedPropertyGroups.Reserve(6);
    SupportedPropertyGroups.Add(MakeShared<FJsonValueString>(TEXT("RenderOpacity")));
    SupportedPropertyGroups.Add(MakeShared<FJsonValueString>(TEXT("RenderTransform.Translation.X/Y via property=transform component=tx/ty")));
    SupportedPropertyGroups.Add(MakeShared<FJsonValueString>(TEXT("RenderTransform.Angle via property=transform component=angle")));
    SupportedPropertyGroups.Add(MakeShared<FJsonValueString>(TEXT("RenderTransform.Scale.X/Y via property=transform component=sx/sy")));
    SupportedPropertyGroups.Add(MakeShared<FJsonValueString>(TEXT("RenderTransform.Shear.X/Y via property=transform component=shx/shy")));
    SupportedPropertyGroups.Add(MakeShared<FJsonValueString>(TEXT("ColorAndOpacity.R/G/B/A via property=color component=r/g/b/a")));

    TArray<TSharedPtr<FJsonValue>> UnsupportedOps;
    UnsupportedOps.Reserve(2);
    UnsupportedOps.Add(MakeShared<FJsonValueString>(TEXT("event key writes require endpoint/function validation and are deferred")));
    UnsupportedOps.Add(MakeShared<FJsonValueString>(TEXT("vector/object track delta is deferred")));

    TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
    Result->SetStringField(TEXT("schema_version"), TEXT("ui_animation_delta.v1"));
    Result->SetStringField(TEXT("asset_path"), AssetPath);
    Result->SetStringField(TEXT("animation_name"), GetAnimationReadableName(Animation));
    Result->SetStringField(TEXT("owner_action"), TEXT("ui.apply_animation_delta"));
    Result->SetArrayField(TEXT("external_aliases_not_registered"), MakeAnimationDeltaExternalAliases());
    Result->SetArrayField(TEXT("supported_operations"), SupportedOps);
    Result->SetArrayField(TEXT("supported_property_groups"), SupportedPropertyGroups);
    Result->SetArrayField(TEXT("unsupported_operations"), UnsupportedOps);
    Result->SetBoolField(TEXT("dry_run"), bDryRun);
    Result->SetBoolField(TEXT("confirmed"), bConfirm);
    Result->SetBoolField(TEXT("confirm_delete"), bConfirmDelete);
    Result->SetBoolField(TEXT("mutated"), bMutated);
    Result->SetBoolField(TEXT("compiled"), bCompiled);
    Result->SetNumberField(TEXT("operations_planned"), Operations.Num());
    Result->SetNumberField(TEXT("operations_applied"), OperationsApplied);
    Result->SetNumberField(TEXT("keys_inserted"), KeysInserted);
    Result->SetNumberField(TEXT("keys_updated"), KeysUpdated);
    Result->SetNumberField(TEXT("keys_deleted"), KeysDeleted);
    Result->SetNumberField(TEXT("bindings_created"), BindingsCreated);
    Result->SetNumberField(TEXT("tracks_created"), TracksCreated);
    Result->SetNumberField(TEXT("sections_created"), SectionsCreated);
    Result->SetNumberField(TEXT("not_found_count"), NotFoundCount);
    Result->SetArrayField(TEXT("operations"), OperationRows);
    if (bReadBack)
    {
        Result->SetObjectField(TEXT("read_back_overview"), MakeAnimationOverview(WBP, Animation));
    }
    return FMonolithActionResult::Success(Result);
}

// --- create_animation ---
FMonolithActionResult FMonolithUIAnimationActions::HandleCreateAnimation(const TSharedPtr<FJsonObject>& Params)
{
    FMonolithActionResult ParamError;
    FString AssetPath;
    if (!MonolithUIInternal::TryGetRequiredString(Params, TEXT("asset_path"), AssetPath, ParamError))
    {
        return ParamError;
    }
    FString AnimationName;
    if (!MonolithUIInternal::TryGetRequiredString(Params, TEXT("animation_name"), AnimationName, ParamError))
    {
        return ParamError;
    }
    double Duration = 0.0;
    if (!Params->TryGetNumberField(TEXT("duration"), Duration))
    {
        return FMonolithActionResult::Error(TEXT("duration is a required number"), -32602);
    }

    if (Duration <= 0.0)
    {
        return FMonolithActionResult::Error(TEXT("Duration must be > 0"));
    }

    FMonolithActionResult Err;
    UWidgetBlueprint* WBP = MonolithUIInternal::LoadWidgetBlueprint(AssetPath, Err);
    if (!WBP) return Err;

    // Check for duplicate name
    for (UWidgetAnimation* Existing : WBP->Animations)
    {
        if (Existing && Existing->GetName() == AnimationName)
        {
            return FMonolithActionResult::Error(
                FString::Printf(TEXT("Animation '%s' already exists"), *AnimationName));
        }
    }

    // Create the UWidgetAnimation as a subobject of the WBP
    UWidgetAnimation* NewAnim = NewObject<UWidgetAnimation>(
        WBP, FName(*AnimationName), RF_Transactional);

    // Create the UMovieScene
    UMovieScene* MovieScene = NewObject<UMovieScene>(
        NewAnim, NewAnim->GetFName(), RF_Transactional);
    NewAnim->MovieScene = MovieScene;

    // Configure tick resolution and display rate
    // UE 5.7 standard: 60000 tick resolution, 30fps display
    FFrameRate TickResolution(60000, 1);
    FFrameRate DisplayRate(30, 1);
    MovieScene->SetTickResolutionDirectly(TickResolution);
    MovieScene->SetDisplayRate(DisplayRate);

    // Set playback range
    FFrameNumber StartFrame(0);
    FFrameNumber EndFrame(FMath::RoundToInt32(Duration * TickResolution.AsDecimal()));
    MovieScene->SetPlaybackRange(
        TRange<FFrameNumber>(StartFrame, EndFrame));

    int32 TrackCount = 0;
    int32 KeyframeCount = 0;

    // Process tracks array if provided
    const TArray<TSharedPtr<FJsonValue>>* TracksArray = nullptr;
    if (Params->TryGetArrayField(TEXT("tracks"), TracksArray) && TracksArray)
    {
        for (const TSharedPtr<FJsonValue>& TrackVal : *TracksArray)
        {
            const TSharedPtr<FJsonObject>* TrackObjPtr = nullptr;
            if (!TrackVal->TryGetObject(TrackObjPtr) || !TrackObjPtr || !(*TrackObjPtr).IsValid())
            {
                continue;
            }
            const TSharedPtr<FJsonObject>& TrackObj = *TrackObjPtr;

            FString WidgetName;
            TrackObj->TryGetStringField(TEXT("widget_name"), WidgetName);
            FString Property;
            TrackObj->TryGetStringField(TEXT("property"), Property);

            if (WidgetName.IsEmpty() || Property.IsEmpty()) continue;

            // Find the widget in the tree
            UWidget* TargetWidget = WBP->WidgetTree
                ? WBP->WidgetTree->FindWidget(FName(*WidgetName))
                : nullptr;
            if (!TargetWidget)
            {
                continue; // Skip tracks for widgets that don't exist
            }

            // Find or create possessable for this widget
            const FGuid PossessableGuid = FindOrCreateWidgetAnimationBinding(WBP, NewAnim, MovieScene, TargetWidget);
            if (!PossessableGuid.IsValid())
            {
                continue;
            }

            // Create a float track for the property
            if (Property == TEXT("opacity"))
            {
                // Create a float track bound to RenderOpacity
                UMovieSceneFloatTrack* FloatTrack = FindOrCreateOpacityTrack(MovieScene, PossessableGuid, StartFrame, EndFrame);
                if (!FloatTrack) continue;

                // Get the float channel and add keyframes
                UMovieSceneSection* Section = FloatTrack->GetAllSections().Num() > 0 ? FloatTrack->GetAllSections()[0] : nullptr;
                if (!Section) continue;

                FMovieSceneChannelProxy& ChannelProxy = Section->GetChannelProxy();
                FMovieSceneFloatChannel* Channel = ChannelProxy.GetChannel<FMovieSceneFloatChannel>(0);
                if (!Channel) continue;

                const TArray<TSharedPtr<FJsonValue>>* KeyframesArray = nullptr;
                if (TrackObj->TryGetArrayField(TEXT("keyframes"), KeyframesArray) && KeyframesArray)
                {
                    for (const TSharedPtr<FJsonValue>& KfVal : *KeyframesArray)
                    {
                        const TSharedPtr<FJsonObject>* KfObjPtr = nullptr;
                        if (!KfVal->TryGetObject(KfObjPtr) || !KfObjPtr || !(*KfObjPtr).IsValid())
                        {
                            continue;
                        }
                        const TSharedPtr<FJsonObject>& KfObj = *KfObjPtr;

                        double Time = 0.0;
                        FMonolithActionResult KeyframeTimeError;
                        if (!TryExtractKeyframeTime(KfObj, Time, KeyframeTimeError))
                        {
                            return KeyframeTimeError;
                        }
                        double Value = 0.0;
                        if (!KfObj->TryGetNumberField(TEXT("value"), Value))
                        {
                            return FMonolithActionResult::Error(TEXT("keyframe.value must be a number"), -32602);
                        }

                        FFrameNumber KeyFrame(
                            FMath::RoundToInt32(Time * TickResolution.AsDecimal()));
                        Channel->AddLinearKey(KeyFrame, static_cast<float>(Value));
                        KeyframeCount++;
                    }
                }

                TrackCount++;
            }
            else if (Property == TEXT("transform"))
            {
                // Transform tracks use MovieScene3DTransformTrack for translation/rotation/scale
                // For UMG we use float tracks on RenderTransform sub-properties
                // Create separate float tracks for TranslationX, TranslationY, Angle, ScaleX, ScaleY

                FString SubProperties[] = {
                    TEXT("RenderTransform.Translation.X"),
                    TEXT("RenderTransform.Translation.Y"),
                    TEXT("RenderTransform.Angle"),
                    TEXT("RenderTransform.Scale.X"),
                    TEXT("RenderTransform.Scale.Y")
                };
                FString SubPropertyNames[] = {
                    TEXT("Translation X"), TEXT("Translation Y"),
                    TEXT("Angle"),
                    TEXT("Scale X"), TEXT("Scale Y")
                };

                // For transform, keyframes contain {time, tx, ty, angle, sx, sy}
                // Create one float track per sub-property, each with matching keyframes
                for (int32 SubIdx = 0; SubIdx < 5; ++SubIdx)
                {
                    UMovieSceneFloatTrack* FloatTrack = MovieScene->AddTrack<UMovieSceneFloatTrack>(PossessableGuid);
                    if (!FloatTrack) continue;

                    FloatTrack->SetPropertyNameAndPath(
                        FName(*SubPropertyNames[SubIdx]), *SubProperties[SubIdx]);

                    UMovieSceneFloatSection* Section = Cast<UMovieSceneFloatSection>(
                        FloatTrack->CreateNewSection());
                    if (!Section) continue;

                    Section->SetRange(TRange<FFrameNumber>(StartFrame, EndFrame));
                    FloatTrack->AddSection(*Section);

                    FMovieSceneChannelProxy& ChannelProxy = Section->GetChannelProxy();
                    FMovieSceneFloatChannel* Channel = ChannelProxy.GetChannel<FMovieSceneFloatChannel>(0);
                    if (!Channel) continue;

                    const TArray<TSharedPtr<FJsonValue>>* KeyframesArray = nullptr;
                    if (TrackObj->TryGetArrayField(TEXT("keyframes"), KeyframesArray) && KeyframesArray)
                    {
                        FString FieldNames[] = {
                            TEXT("tx"), TEXT("ty"), TEXT("angle"), TEXT("sx"), TEXT("sy")
                        };
                        double Defaults[] = { 0.0, 0.0, 0.0, 1.0, 1.0 };

                        for (const TSharedPtr<FJsonValue>& KfVal : *KeyframesArray)
                        {
                            const TSharedPtr<FJsonObject>* KfObjPtr = nullptr;
                            if (!KfVal->TryGetObject(KfObjPtr) || !KfObjPtr || !(*KfObjPtr).IsValid())
                            {
                                continue;
                            }
                            const TSharedPtr<FJsonObject>& KfObj = *KfObjPtr;

                            double Time = 0.0;
                            FMonolithActionResult KeyframeTimeError;
                            if (!TryExtractKeyframeTime(KfObj, Time, KeyframeTimeError))
                            {
                                return KeyframeTimeError;
                            }
                            double Value = Defaults[SubIdx];
                            const TSharedPtr<FJsonValue> ValField = KfObj->TryGetField(FieldNames[SubIdx]);
                            if (ValField.IsValid())
                            {
                                if (!ValField->TryGetNumber(Value))
                                {
                                    return FMonolithActionResult::Error(
                                        FString::Printf(TEXT("keyframe.%s must be a number"), *FieldNames[SubIdx]), -32602);
                                }
                            }

                            FFrameNumber KeyFrame(
                                FMath::RoundToInt32(Time * TickResolution.AsDecimal()));
                            Channel->AddLinearKey(KeyFrame, static_cast<float>(Value));
                            KeyframeCount++;
                        }
                    }

                    TrackCount++;
                }
            }
            else if (Property == TEXT("color"))
            {
                // Color tracks: R, G, B, A float channels on ColorAndOpacity
                FString SubProperties[] = {
                    TEXT("ColorAndOpacity.R"),
                    TEXT("ColorAndOpacity.G"),
                    TEXT("ColorAndOpacity.B"),
                    TEXT("ColorAndOpacity.A")
                };
                FString SubPropertyNames[] = {
                    TEXT("Color R"), TEXT("Color G"), TEXT("Color B"), TEXT("Color A")
                };

                for (int32 SubIdx = 0; SubIdx < 4; ++SubIdx)
                {
                    UMovieSceneFloatTrack* FloatTrack = MovieScene->AddTrack<UMovieSceneFloatTrack>(PossessableGuid);
                    if (!FloatTrack) continue;

                    FloatTrack->SetPropertyNameAndPath(
                        FName(*SubPropertyNames[SubIdx]), *SubProperties[SubIdx]);

                    UMovieSceneFloatSection* Section = Cast<UMovieSceneFloatSection>(
                        FloatTrack->CreateNewSection());
                    if (!Section) continue;

                    Section->SetRange(TRange<FFrameNumber>(StartFrame, EndFrame));
                    FloatTrack->AddSection(*Section);

                    FMovieSceneChannelProxy& ChannelProxy = Section->GetChannelProxy();
                    FMovieSceneFloatChannel* Channel = ChannelProxy.GetChannel<FMovieSceneFloatChannel>(0);
                    if (!Channel) continue;

                    const TArray<TSharedPtr<FJsonValue>>* KeyframesArray = nullptr;
                    if (TrackObj->TryGetArrayField(TEXT("keyframes"), KeyframesArray) && KeyframesArray)
                    {
                        FString FieldNames[] = {
                            TEXT("r"), TEXT("g"), TEXT("b"), TEXT("a")
                        };
                        double Defaults[] = { 1.0, 1.0, 1.0, 1.0 };

                        for (const TSharedPtr<FJsonValue>& KfVal : *KeyframesArray)
                        {
                            const TSharedPtr<FJsonObject>* KfObjPtr = nullptr;
                            if (!KfVal->TryGetObject(KfObjPtr) || !KfObjPtr || !(*KfObjPtr).IsValid())
                            {
                                continue;
                            }
                            const TSharedPtr<FJsonObject>& KfObj = *KfObjPtr;

                            double Time = 0.0;
                            FMonolithActionResult KeyframeTimeError;
                            if (!TryExtractKeyframeTime(KfObj, Time, KeyframeTimeError))
                            {
                                return KeyframeTimeError;
                            }
                            double Value = Defaults[SubIdx];
                            const TSharedPtr<FJsonValue> ValField = KfObj->TryGetField(FieldNames[SubIdx]);
                            if (ValField.IsValid())
                            {
                                if (!ValField->TryGetNumber(Value))
                                {
                                    return FMonolithActionResult::Error(
                                        FString::Printf(TEXT("keyframe.%s must be a number"), *FieldNames[SubIdx]), -32602);
                                }
                            }

                            FFrameNumber KeyFrame(
                                FMath::RoundToInt32(Time * TickResolution.AsDecimal()));
                            Channel->AddLinearKey(KeyFrame, static_cast<float>(Value));
                            KeyframeCount++;
                        }
                    }

                    TrackCount++;
                }
            }
            else
            {
                // Unknown property — skip with no error (lenient)
                continue;
            }
        }
    }

    // Add the animation to the widget blueprint
    WBP->Animations.Add(NewAnim);

    // Mirror editor bookkeeping so the compiler sees a GUID for the final animation name.
    MonolithUIInternal::RegisterVariableName(WBP, NewAnim->GetFName());

    // Mark modified and compile
    FBlueprintEditorUtils::MarkBlueprintAsStructurallyModified(WBP);
    FKismetEditorUtilities::CompileBlueprint(WBP);

    TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
    Result->SetStringField(TEXT("asset_path"), AssetPath);
    Result->SetStringField(TEXT("animation_name"), AnimationName);
    Result->SetNumberField(TEXT("duration"), Duration);
    Result->SetNumberField(TEXT("tracks_created"), TrackCount);
    Result->SetNumberField(TEXT("keyframes_created"), KeyframeCount);
    Result->SetBoolField(TEXT("compiled"), true);
    // Phase L deprecation hint -- v1 surface scheduled for removal one major release out.
    // Same {deprecated, use_action} payload shape used elsewhere in the registry for
    // cross-namespace alias deprecation, so callers can branch on a single tag.
    Result->SetBoolField(TEXT("deprecated"), true);
    Result->SetStringField(TEXT("use_action"), TEXT("ui::create_animation_v2"));
    return FMonolithActionResult::Success(Result);
}

// --- add_animation_keyframe ---
FMonolithActionResult FMonolithUIAnimationActions::HandleAddAnimationKeyframe(const TSharedPtr<FJsonObject>& Params)
{
    FMonolithActionResult ParamError;
    FString AssetPath;
    if (!MonolithUIInternal::TryGetRequiredString(Params, TEXT("asset_path"), AssetPath, ParamError))
    {
        return ParamError;
    }
    FString AnimationName;
    if (!MonolithUIInternal::TryGetRequiredString(Params, TEXT("animation_name"), AnimationName, ParamError))
    {
        return ParamError;
    }
    FString WidgetName;
    if (!MonolithUIInternal::TryGetRequiredString(Params, TEXT("widget_name"), WidgetName, ParamError))
    {
        return ParamError;
    }
    FString Property;
    if (!MonolithUIInternal::TryGetRequiredString(Params, TEXT("property"), Property, ParamError))
    {
        return ParamError;
    }
    FString Component;
    const TSharedPtr<FJsonValue> CompField = Params->TryGetField(TEXT("component"));
    if (CompField.IsValid())
    {
        if (!CompField->TryGetString(Component))
        {
            return FMonolithActionResult::Error(TEXT("component must be a string"), -32602);
        }
    }
    double Time = 0.0;
    if (!Params->TryGetNumberField(TEXT("time"), Time))
    {
        return FMonolithActionResult::Error(TEXT("time must be a number"), -32602);
    }
    double Value = 0.0;
    if (!Params->TryGetNumberField(TEXT("value"), Value))
    {
        return FMonolithActionResult::Error(TEXT("value must be a number"), -32602);
    }

    FMonolithActionResult Err;
    UWidgetBlueprint* WBP = MonolithUIInternal::LoadWidgetBlueprint(AssetPath, Err);
    if (!WBP) return Err;

    // Find the animation
    UWidgetAnimation* TargetAnim = nullptr;
    for (UWidgetAnimation* Anim : WBP->Animations)
    {
        if (Anim && Anim->GetName() == AnimationName)
        {
            TargetAnim = Anim;
            break;
        }
    }
    if (!TargetAnim)
    {
        return FMonolithActionResult::Error(
            FString::Printf(TEXT("Animation '%s' not found"), *AnimationName));
    }

    UMovieScene* MovieScene = TargetAnim->GetMovieScene();
    if (!MovieScene)
    {
        return FMonolithActionResult::Error(TEXT("Animation has no MovieScene"));
    }

    FFrameRate TickRes = MovieScene->GetTickResolution();
    FFrameNumber KeyFrame(FMath::RoundToInt32(Time * TickRes.AsDecimal()));

    // Determine the property path to match
    FString PropertyPath;
    FName TrackPropertyName = NAME_None;
    if (Property == TEXT("opacity"))
    {
        PropertyPath = TEXT("RenderOpacity");
        TrackPropertyName = FName(TEXT("RenderOpacity"));
    }
    else if (Property == TEXT("transform"))
    {
        if (Component == TEXT("tx"))
        {
            PropertyPath = TEXT("RenderTransform.Translation.X");
            TrackPropertyName = FName(TEXT("Translation X"));
        }
        else if (Component == TEXT("ty"))
        {
            PropertyPath = TEXT("RenderTransform.Translation.Y");
            TrackPropertyName = FName(TEXT("Translation Y"));
        }
        else if (Component == TEXT("angle"))
        {
            PropertyPath = TEXT("RenderTransform.Angle");
            TrackPropertyName = FName(TEXT("Angle"));
        }
        else if (Component == TEXT("sx"))
        {
            PropertyPath = TEXT("RenderTransform.Scale.X");
            TrackPropertyName = FName(TEXT("Scale X"));
        }
        else if (Component == TEXT("sy"))
        {
            PropertyPath = TEXT("RenderTransform.Scale.Y");
            TrackPropertyName = FName(TEXT("Scale Y"));
        }
        else
        {
            return FMonolithActionResult::Error(
                TEXT("For property 'transform', component must be one of: tx, ty, angle, sx, sy"));
        }
    }
    else if (Property == TEXT("color"))
    {
        if (Component == TEXT("r"))
        {
            PropertyPath = TEXT("ColorAndOpacity.R");
            TrackPropertyName = FName(TEXT("Color R"));
        }
        else if (Component == TEXT("g"))
        {
            PropertyPath = TEXT("ColorAndOpacity.G");
            TrackPropertyName = FName(TEXT("Color G"));
        }
        else if (Component == TEXT("b"))
        {
            PropertyPath = TEXT("ColorAndOpacity.B");
            TrackPropertyName = FName(TEXT("Color B"));
        }
        else if (Component == TEXT("a"))
        {
            PropertyPath = TEXT("ColorAndOpacity.A");
            TrackPropertyName = FName(TEXT("Color A"));
        }
        else
        {
            return FMonolithActionResult::Error(
                TEXT("For property 'color', component must be one of: r, g, b, a"));
        }
    }
    else
    {
        return FMonolithActionResult::Error(
            TEXT("add_animation_keyframe supports properties: opacity, transform, color"));
    }

    UWidget* TargetWidget = WBP->WidgetTree ? WBP->WidgetTree->FindWidget(FName(*WidgetName)) : nullptr;
    if (!TargetWidget)
    {
        return FMonolithActionResult::Error(
            FString::Printf(TEXT("Widget '%s' not found"), *WidgetName));
    }

    const TRange<FFrameNumber> PlaybackRange = MovieScene->GetPlaybackRange();
    const FFrameNumber StartFrame = PlaybackRange.GetLowerBoundValue();
    const FFrameNumber EndFrame = PlaybackRange.GetUpperBoundValue();
    const FGuid PossessableGuid = FindOrCreateWidgetAnimationBinding(WBP, TargetAnim, MovieScene, TargetWidget);
    if (!PossessableGuid.IsValid())
    {
        return FMonolithActionResult::Error(
            FString::Printf(TEXT("Unable to create animation binding for widget '%s'"), *WidgetName));
    }

    UMovieSceneFloatTrack* FloatTrack = FindOrCreateFloatTrack(
        MovieScene,
        PossessableGuid,
        TrackPropertyName,
        PropertyPath,
        StartFrame,
        EndFrame);
    if (!FloatTrack)
    {
        return FMonolithActionResult::Error(
            FString::Printf(TEXT("Unable to create %s track for widget '%s'"), *PropertyPath, *WidgetName));
    }

    for (UMovieSceneSection* Section : FloatTrack->GetAllSections())
    {
        FMovieSceneChannelProxy& ChannelProxy = Section->GetChannelProxy();
        FMovieSceneFloatChannel* Channel = ChannelProxy.GetChannel<FMovieSceneFloatChannel>(0);
        if (!Channel)
        {
            continue;
        }

        Channel->AddLinearKey(KeyFrame, static_cast<float>(Value));

        FBlueprintEditorUtils::MarkBlueprintAsStructurallyModified(WBP);
        FKismetEditorUtilities::CompileBlueprint(WBP);

        TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
        Result->SetStringField(TEXT("asset_path"), AssetPath);
        Result->SetStringField(TEXT("animation_name"), AnimationName);
        Result->SetStringField(TEXT("widget_name"), WidgetName);
        Result->SetStringField(TEXT("property"), Property);
        if (!Component.IsEmpty())
        {
            Result->SetStringField(TEXT("component"), Component);
        }
        Result->SetNumberField(TEXT("time"), Time);
        Result->SetNumberField(TEXT("value"), Value);
        Result->SetBoolField(TEXT("binding_created"), true);
        Result->SetBoolField(TEXT("track_created"), true);
        Result->SetBoolField(TEXT("added"), true);
        // Phase L deprecation hint -- v1 surface scheduled for removal one major release out.
        Result->SetBoolField(TEXT("deprecated"), true);
        Result->SetStringField(TEXT("use_action"), TEXT("ui::create_animation_v2"));
        return FMonolithActionResult::Success(Result);
    }

    return FMonolithActionResult::Error(
        FString::Printf(TEXT("No section channel found for property '%s' on widget '%s'"), *Property, *WidgetName));
}

// --- remove_animation ---
FMonolithActionResult FMonolithUIAnimationActions::HandleRemoveAnimation(const TSharedPtr<FJsonObject>& Params)
{
    FMonolithActionResult Err;
    FString AssetPath;
    if (!MonolithUIInternal::TryGetRequiredString(Params, TEXT("asset_path"), AssetPath, Err))
    {
        return Err;
    }
    FString AnimationName;
    if (!MonolithUIInternal::TryGetRequiredString(Params, TEXT("animation_name"), AnimationName, Err))
    {
        return Err;
    }

    UWidgetBlueprint* WBP = MonolithUIInternal::LoadWidgetBlueprint(AssetPath, Err);
    if (!WBP) return Err;

    // Find and remove the animation
    int32 FoundIndex = INDEX_NONE;
    for (int32 i = 0; i < WBP->Animations.Num(); ++i)
    {
        if (WBP->Animations[i] && WBP->Animations[i]->GetName() == AnimationName)
        {
            FoundIndex = i;

            break;
        }
    }

    if (FoundIndex == INDEX_NONE)
    {
        return FMonolithActionResult::Error(
            FString::Printf(TEXT("Animation '%s' not found"), *AnimationName));
    }

    // AnimationBindings live on the UWidgetAnimation itself, not the WBP.
    // Since we're removing the animation entirely, its bindings go with it.

    if (WBP->WidgetVariableNameToGuidMap.Contains(FName(*AnimationName)))
    {
        WBP->OnVariableRemoved(FName(*AnimationName));
    }

    // Remove the animation object
    WBP->Animations.RemoveAt(FoundIndex);

    FBlueprintEditorUtils::MarkBlueprintAsStructurallyModified(WBP);
    FKismetEditorUtilities::CompileBlueprint(WBP);

    TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
    Result->SetStringField(TEXT("asset_path"), AssetPath);
    Result->SetStringField(TEXT("animation_name"), AnimationName);
    Result->SetBoolField(TEXT("removed"), true);
    Result->SetBoolField(TEXT("compiled"), true);
    return FMonolithActionResult::Success(Result);
}
