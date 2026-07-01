// MonolithUIActions.cpp
#include "MonolithUIActions.h"
#include "MonolithUIInternal.h"
#include "MonolithParamSchema.h"
#include "MonolithPackagePathValidator.h"
#include "WidgetBlueprintFactory.h"
#include "AssetRegistry/AssetRegistryModule.h"
#include "UObject/SavePackage.h"
#include "MonolithAssetUtils.h"
#include "Misc/ConfigCacheIni.h"
#include "Misc/PackageName.h"
#include "GameplayTagContainer.h"
#include "Interfaces/IPluginManager.h"
#include "Modules/ModuleManager.h"
#include "UObject/UObjectIterator.h"
#include "UObject/UnrealType.h"

// Phase C: set_widget_property routes through the allowlist-gated reflection
// helper. The legacy bare-FProperty::ImportText_Direct path is preserved
// behind the `raw_mode=true` opt-out so existing call sites that previously
// relied on writing arbitrary properties unconditionally still work.
#include "Editor.h"
#include "Registry/MonolithUIRegistrySubsystem.h"
#include "Registry/UIPropertyAllowlist.h"
#include "Registry/UIPropertyPathCache.h"
#include "Registry/UIReflectionHelper.h"

// Bug #5 fix (2026-05-16 UI gap audit): compile_widget now surfaces
// FCompilerResultsLog messages as errors[]/warnings[] arrays in the response
// payload, matching the shape blueprint_query("compile_blueprint") returns.
// FCompilerResultsLog is the canonical channel — IWidgetCompilerLog (which
// UCommonBoundActionBar::ValidateCompiledDefaults writes through) routes back
// into the same results log for widget BPs, so capturing here covers both
// the compiler-graph errors AND the validator-emitted ones.
#include "Kismet2/CompilerResultsLog.h"
#include "Kismet2/KismetEditorUtilities.h"
#include "Logging/TokenizedMessage.h"

// Phase 2 (2026-05-16 UI gap audit): rename_widget + dump_blueprint_compile_log.
// rename_widget recompiles via FBlueprintCompilationManager so the Skeleton class
// stamping (BPVAR rename in NewVariables[]) survives the post-compile reflection
// walk that get_widget_tree / set_widget_property uses.
#include "Kismet2/BlueprintEditorUtils.h"
#include "Engine/Blueprint.h"

// Phase 2 (Item #7 / #14) — file-static handler forward declarations. Lives
// here rather than as static members on FMonolithUIActions because the plan
// (Phase 2 §F6) prohibits header changes; we register through the class's
// existing RegisterActions hook and dispatch into these file-static handlers.
namespace MonolithUIActionsPhase2
{
    static FMonolithActionResult HandleRenameWidget(const TSharedPtr<FJsonObject>& Params);
    static FMonolithActionResult HandleDumpBlueprintCompileLog(const TSharedPtr<FJsonObject>& Params);
}

namespace MonolithUISetWidgetPropertyInternal
{
    static bool IsVariableFlagProperty(const FString& PropertyName)
    {
        return PropertyName.Equals(TEXT("IsVariable"), ESearchCase::IgnoreCase)
            || PropertyName.Equals(TEXT("bIsVariable"), ESearchCase::IgnoreCase);
    }

    static bool TryReadBoolValue(const TSharedPtr<FJsonValue>& ValueJson, bool& OutValue, FString& OutError)
    {
        if (!ValueJson.IsValid())
        {
            OutError = TEXT("missing JSON value");
            return false;
        }

        if (ValueJson->Type == EJson::Boolean)
        {
            return ValueJson->TryGetBool(OutValue);
        }

        FString TextValue;
        if (ValueJson->TryGetString(TextValue))
        {
            TextValue.TrimStartAndEndInline();
            if (TextValue.Equals(TEXT("true"), ESearchCase::IgnoreCase) || TextValue == TEXT("1"))
            {
                OutValue = true;
                return true;
            }
            if (TextValue.Equals(TEXT("false"), ESearchCase::IgnoreCase) || TextValue == TEXT("0"))
            {
                OutValue = false;
                return true;
            }

            OutError = FString::Printf(TEXT("string value '%s' is not one of true/false/1/0"), *TextValue);
            return false;
        }

        double NumberValue = 0.0;
        if (ValueJson->TryGetNumber(NumberValue))
        {
            if (NumberValue == 0.0)
            {
                OutValue = false;
                return true;
            }
            if (NumberValue == 1.0)
            {
                OutValue = true;
                return true;
            }

            OutError = FString::Printf(TEXT("numeric value %.17g is not 0 or 1"), NumberValue);
            return false;
        }

        OutError = TEXT("expected a boolean, true/false string, or 0/1 number");
        return false;
    }
}

namespace MonolithUICommonFrameworkInternal
{
    struct FCommonClassSpec
    {
        const TCHAR* Name;
        const TCHAR* Path;
        const TCHAR* Plugin;
    };

    struct FCommonStructSpec
    {
        const TCHAR* Name;
        const TCHAR* Path;
        const TCHAR* Plugin;
    };

    static const FCommonClassSpec CommonClassSpecs[] =
    {
        { TEXT("CommonUI.CommonActivatableWidget"), TEXT("/Script/CommonUI.CommonActivatableWidget"), TEXT("CommonUI") },
        { TEXT("CommonUI.CommonActivatableWidgetContainerBase"), TEXT("/Script/CommonUI.CommonActivatableWidgetContainerBase"), TEXT("CommonUI") },
        { TEXT("CommonUI.CommonUserWidget"), TEXT("/Script/CommonUI.CommonUserWidget"), TEXT("CommonUI") },
        { TEXT("CommonGame.GameUIManagerSubsystem"), TEXT("/Script/CommonGame.GameUIManagerSubsystem"), TEXT("CommonGame") },
        { TEXT("CommonGame.GameUIPolicy"), TEXT("/Script/CommonGame.GameUIPolicy"), TEXT("CommonGame") },
        { TEXT("CommonGame.PrimaryGameLayout"), TEXT("/Script/CommonGame.PrimaryGameLayout"), TEXT("CommonGame") },
        { TEXT("CommonGame.CommonMessagingSubsystem"), TEXT("/Script/CommonGame.CommonMessagingSubsystem"), TEXT("CommonGame") },
        { TEXT("CommonGame.CommonGameDialog"), TEXT("/Script/CommonGame.CommonGameDialog"), TEXT("CommonGame") },
        { TEXT("CommonGame.CommonGameDialogDescriptor"), TEXT("/Script/CommonGame.CommonGameDialogDescriptor"), TEXT("CommonGame") },
        { TEXT("UIExtension.UIExtensionSubsystem"), TEXT("/Script/UIExtension.UIExtensionSubsystem"), TEXT("UIExtension") },
        { TEXT("UIExtension.UIExtensionPointWidget"), TEXT("/Script/UIExtension.UIExtensionPointWidget"), TEXT("UIExtension") },
        { TEXT("CommonUser.CommonUserSubsystem"), TEXT("/Script/CommonUser.CommonUserSubsystem"), TEXT("CommonUser") },
        { TEXT("CommonUser.CommonSessionSubsystem"), TEXT("/Script/CommonUser.CommonSessionSubsystem"), TEXT("CommonUser") },
        { TEXT("CommonLoadingScreen.LoadingScreenManager"), TEXT("/Script/CommonLoadingScreen.LoadingScreenManager"), TEXT("CommonLoadingScreen") },
        { TEXT("CommonLoadingScreen.LoadingProcessInterface"), TEXT("/Script/CommonLoadingScreen.LoadingProcessInterface"), TEXT("CommonLoadingScreen") },
        { TEXT("CommonLoadingScreen.LoadingProcessTask"), TEXT("/Script/CommonLoadingScreen.LoadingProcessTask"), TEXT("CommonLoadingScreen") },
        { TEXT("CommonLoadingScreen.CommonLoadingScreenSettings"), TEXT("/Script/CommonLoadingScreen.CommonLoadingScreenSettings"), TEXT("CommonLoadingScreen") },
        { TEXT("GameSettings.GameSetting"), TEXT("/Script/GameSettings.GameSetting"), TEXT("GameSettings") },
        { TEXT("GameSettings.GameSettingRegistry"), TEXT("/Script/GameSettings.GameSettingRegistry"), TEXT("GameSettings") },
        { TEXT("GameSettings.GameSettingCollection"), TEXT("/Script/GameSettings.GameSettingCollection"), TEXT("GameSettings") },
        { TEXT("GameSettings.GameSettingCollectionPage"), TEXT("/Script/GameSettings.GameSettingCollectionPage"), TEXT("GameSettings") },
        { TEXT("GameSettings.GameSettingValue"), TEXT("/Script/GameSettings.GameSettingValue"), TEXT("GameSettings") },
        { TEXT("GameSettings.GameSettingAction"), TEXT("/Script/GameSettings.GameSettingAction"), TEXT("GameSettings") },
        { TEXT("GameSettings.GameSettingScreen"), TEXT("/Script/GameSettings.GameSettingScreen"), TEXT("GameSettings") },
        { TEXT("GameSettings.GameSettingPanel"), TEXT("/Script/GameSettings.GameSettingPanel"), TEXT("GameSettings") },
        { TEXT("GameSettings.GameSettingVisualData"), TEXT("/Script/GameSettings.GameSettingVisualData"), TEXT("GameSettings") },
        { TEXT("GameplayMessageRuntime.GameplayMessageSubsystem"), TEXT("/Script/GameplayMessageRuntime.GameplayMessageSubsystem"), TEXT("GameplayMessageRouter") },
        { TEXT("GameplayMessageRuntime.AsyncAction_ListenForGameplayMessage"), TEXT("/Script/GameplayMessageRuntime.AsyncAction_ListenForGameplayMessage"), TEXT("GameplayMessageRouter") },
        { TEXT("ModularGameplayActors.ModularPawn"), TEXT("/Script/ModularGameplayActors.ModularPawn"), TEXT("ModularGameplayActors") },
        { TEXT("ModularGameplayActors.ModularCharacter"), TEXT("/Script/ModularGameplayActors.ModularCharacter"), TEXT("ModularGameplayActors") },
        { TEXT("ModularGameplayActors.ModularPlayerController"), TEXT("/Script/ModularGameplayActors.ModularPlayerController"), TEXT("ModularGameplayActors") },
        { TEXT("ModularGameplayActors.ModularPlayerState"), TEXT("/Script/ModularGameplayActors.ModularPlayerState"), TEXT("ModularGameplayActors") },
        { TEXT("ModularGameplayActors.ModularGameStateBase"), TEXT("/Script/ModularGameplayActors.ModularGameStateBase"), TEXT("ModularGameplayActors") },
        { TEXT("ModularGameplayActors.ModularGameState"), TEXT("/Script/ModularGameplayActors.ModularGameState"), TEXT("ModularGameplayActors") },
        { TEXT("ModularGameplayActors.ModularGameModeBase"), TEXT("/Script/ModularGameplayActors.ModularGameModeBase"), TEXT("ModularGameplayActors") },
        { TEXT("ModularGameplayActors.ModularGameMode"), TEXT("/Script/ModularGameplayActors.ModularGameMode"), TEXT("ModularGameplayActors") },
        { TEXT("ModularGameplayActors.ModularAIController"), TEXT("/Script/ModularGameplayActors.ModularAIController"), TEXT("ModularGameplayActors") },
        { TEXT("GameSubtitles.SubtitleDisplaySubsystem"), TEXT("/Script/GameSubtitles.SubtitleDisplaySubsystem"), TEXT("GameSubtitles") },
        { TEXT("GameSubtitles.SubtitleDisplay"), TEXT("/Script/GameSubtitles.SubtitleDisplay"), TEXT("GameSubtitles") },
        { TEXT("GameSubtitles.SubtitleDisplayOptions"), TEXT("/Script/GameSubtitles.SubtitleDisplayOptions"), TEXT("GameSubtitles") },
        { TEXT("GameSubtitles.MediaSubtitlesPlayer"), TEXT("/Script/GameSubtitles.MediaSubtitlesPlayer"), TEXT("GameSubtitles") }
    };

    static const FCommonStructSpec CommonStructSpecs[] =
    {
        { TEXT("GameplayMessageRuntime.GameplayMessageListenerHandle"), TEXT("/Script/GameplayMessageRuntime.GameplayMessageListenerHandle"), TEXT("GameplayMessageRouter") },
        { TEXT("GameplayMessageRuntime.GameplayMessageListenerData"), TEXT("/Script/GameplayMessageRuntime.GameplayMessageListenerData"), TEXT("GameplayMessageRouter") },
        { TEXT("GameSettings.GameSettingFilterState"), TEXT("/Script/GameSettings.GameSettingFilterState"), TEXT("GameSettings") },
        { TEXT("GameSettings.GameSettingClassExtensions"), TEXT("/Script/GameSettings.GameSettingClassExtensions"), TEXT("GameSettings") },
        { TEXT("GameSettings.GameSettingNameExtensions"), TEXT("/Script/GameSettings.GameSettingNameExtensions"), TEXT("GameSettings") },
        { TEXT("GameSubtitles.SubtitleFormat"), TEXT("/Script/GameSubtitles.SubtitleFormat"), TEXT("GameSubtitles") }
    };

    static UClass* LoadClassPath(const TCHAR* ClassPath)
    {
        return StaticLoadClass(UObject::StaticClass(), nullptr, ClassPath);
    }

    static UScriptStruct* LoadStructPath(const TCHAR* StructPath)
    {
        return LoadObject<UScriptStruct>(nullptr, StructPath);
    }

    static int32 GetOptionalInt(const TSharedPtr<FJsonObject>& Params, const TCHAR* FieldName, int32 DefaultValue, int32 MinValue, int32 MaxValue)
    {
        if (!Params.IsValid())
        {
            return DefaultValue;
        }

        double RawValue = DefaultValue;
        if (!Params->TryGetNumberField(FieldName, RawValue))
        {
            return DefaultValue;
        }

        const int32 Value = FMath::RoundToInt(RawValue);
        return FMath::Clamp(Value, MinValue, MaxValue);
    }

    static FString ClassPath(const UClass* Class)
    {
        return Class ? Class->GetPathName() : FString();
    }

    static FString ExportPropertyValue(const FProperty* Property, const void* Container)
    {
        if (!Property || !Container)
        {
            return FString();
        }

        FString Value;
        const void* ValuePtr = Property->ContainerPtrToValuePtr<void>(Container);
        Property->ExportText_Direct(Value, ValuePtr, nullptr, nullptr, PPF_None);
        return Value;
    }

    static TSharedPtr<FJsonObject> PluginStatus(const TCHAR* PluginName)
    {
        TSharedPtr<FJsonObject> Obj = MakeShared<FJsonObject>();
        Obj->SetStringField(TEXT("name"), PluginName);

        const TSharedPtr<IPlugin> Plugin = IPluginManager::Get().FindPlugin(PluginName);
        Obj->SetBoolField(TEXT("found"), Plugin.IsValid());
        if (Plugin.IsValid())
        {
            Obj->SetBoolField(TEXT("enabled"), Plugin->IsEnabled());
            Obj->SetStringField(TEXT("base_dir"), Plugin->GetBaseDir());
            Obj->SetBoolField(TEXT("can_contain_content"), Plugin->CanContainContent());
            Obj->SetStringField(TEXT("version_name"), Plugin->GetDescriptor().VersionName);
        }
        return Obj;
    }

    static TSharedPtr<FJsonObject> ModuleStatus(const TCHAR* ModuleName)
    {
        TSharedPtr<FJsonObject> Obj = MakeShared<FJsonObject>();
        Obj->SetStringField(TEXT("name"), ModuleName);
        const FName Name(ModuleName);
        Obj->SetBoolField(TEXT("exists"), FModuleManager::Get().ModuleExists(ModuleName));
        Obj->SetBoolField(TEXT("loaded"), FModuleManager::Get().IsModuleLoaded(Name));
        return Obj;
    }

    static TSharedPtr<FJsonObject> PropertySummary(const FProperty* Property)
    {
        TSharedPtr<FJsonObject> Obj = MakeShared<FJsonObject>();
        Obj->SetStringField(TEXT("name"), Property ? Property->GetName() : FString());
        Obj->SetStringField(TEXT("type"), Property ? Property->GetCPPType() : FString());
        if (!Property)
        {
            return Obj;
        }

        Obj->SetBoolField(TEXT("editable"), Property->HasAnyPropertyFlags(CPF_Edit));
        Obj->SetBoolField(TEXT("blueprint_visible"), Property->HasAnyPropertyFlags(CPF_BlueprintVisible));
        Obj->SetBoolField(TEXT("config"), Property->HasAnyPropertyFlags(CPF_Config));
        Obj->SetBoolField(TEXT("transient"), Property->HasAnyPropertyFlags(CPF_Transient));

        if (const FClassProperty* ClassProperty = CastField<FClassProperty>(Property))
        {
            Obj->SetStringField(TEXT("meta_class"), ClassPath(ClassProperty->MetaClass));
        }
        else if (const FObjectPropertyBase* ObjectProperty = CastField<FObjectPropertyBase>(Property))
        {
            Obj->SetStringField(TEXT("property_class"), ClassPath(ObjectProperty->PropertyClass));
        }
        else if (const FStructProperty* StructProperty = CastField<FStructProperty>(Property))
        {
            Obj->SetStringField(TEXT("struct"), StructProperty->Struct ? StructProperty->Struct->GetPathName() : FString());
        }
        else if (const FEnumProperty* EnumProperty = CastField<FEnumProperty>(Property))
        {
            Obj->SetStringField(TEXT("enum"), EnumProperty->GetEnum() ? EnumProperty->GetEnum()->GetPathName() : FString());
        }
        else if (const FByteProperty* ByteProperty = CastField<FByteProperty>(Property))
        {
            if (ByteProperty->Enum)
            {
                Obj->SetStringField(TEXT("enum"), ByteProperty->Enum->GetPathName());
            }
        }

        return Obj;
    }

    static TSharedPtr<FJsonObject> FunctionSummary(const UFunction* Function)
    {
        TSharedPtr<FJsonObject> Obj = MakeShared<FJsonObject>();
        Obj->SetStringField(TEXT("name"), Function ? Function->GetName() : FString());
        if (!Function)
        {
            return Obj;
        }

        Obj->SetBoolField(TEXT("blueprint_callable"), Function->HasAnyFunctionFlags(FUNC_BlueprintCallable));
        Obj->SetBoolField(TEXT("blueprint_pure"), Function->HasAnyFunctionFlags(FUNC_BlueprintPure));
        Obj->SetBoolField(TEXT("static"), Function->HasAnyFunctionFlags(FUNC_Static));
        Obj->SetBoolField(TEXT("exec"), Function->HasAnyFunctionFlags(FUNC_Exec));
        if (Function->HasMetaData(TEXT("Category")))
        {
            Obj->SetStringField(TEXT("category"), Function->GetMetaData(TEXT("Category")));
        }
        return Obj;
    }

    static TSharedPtr<FJsonObject> ClassSummary(const FCommonClassSpec& Spec, bool bIncludeProperties, bool bIncludeFunctions, int32 PropertyLimit, int32 FunctionLimit)
    {
        TSharedPtr<FJsonObject> Obj = MakeShared<FJsonObject>();
        Obj->SetStringField(TEXT("name"), Spec.Name);
        Obj->SetStringField(TEXT("path"), Spec.Path);
        Obj->SetStringField(TEXT("plugin"), Spec.Plugin);

        UClass* Class = LoadClassPath(Spec.Path);
        Obj->SetBoolField(TEXT("available"), Class != nullptr);
        if (!Class)
        {
            return Obj;
        }

        Obj->SetStringField(TEXT("class_path"), Class->GetPathName());
        Obj->SetStringField(TEXT("super_class"), ClassPath(Class->GetSuperClass()));
        Obj->SetBoolField(TEXT("abstract"), Class->HasAnyClassFlags(CLASS_Abstract));
        Obj->SetBoolField(TEXT("native"), Class->HasAnyClassFlags(CLASS_Native));
        Obj->SetBoolField(TEXT("blueprint_type"), Class->HasMetaData(TEXT("BlueprintType")));
        Obj->SetBoolField(TEXT("blueprintable"), FKismetEditorUtilities::CanCreateBlueprintOfClass(Class));

        if (bIncludeProperties)
        {
            TArray<TSharedPtr<FJsonValue>> Properties;
            int32 Added = 0;
            int32 Total = 0;
            for (TFieldIterator<FProperty> It(Class, EFieldIteratorFlags::IncludeSuper); It; ++It)
            {
                ++Total;
                const FProperty* Property = *It;
                if (!Property || !(Property->HasAnyPropertyFlags(CPF_Edit) || Property->HasAnyPropertyFlags(CPF_BlueprintVisible) || Property->HasAnyPropertyFlags(CPF_Config)))
                {
                    continue;
                }
                if (Added < PropertyLimit)
                {
                    Properties.Add(MakeShared<FJsonValueObject>(PropertySummary(Property)));
                    ++Added;
                }
            }
            Obj->SetArrayField(TEXT("properties"), Properties);
            Obj->SetNumberField(TEXT("property_count"), Total);
            Obj->SetBoolField(TEXT("properties_truncated"), Added >= PropertyLimit);
        }

        if (bIncludeFunctions)
        {
            TArray<TSharedPtr<FJsonValue>> Functions;
            int32 Added = 0;
            int32 Total = 0;
            for (TFieldIterator<UFunction> It(Class, EFieldIteratorFlags::IncludeSuper); It; ++It)
            {
                ++Total;
                const UFunction* Function = *It;
                if (!Function || !(Function->HasAnyFunctionFlags(FUNC_BlueprintCallable) || Function->HasAnyFunctionFlags(FUNC_BlueprintPure)))
                {
                    continue;
                }
                if (Added < FunctionLimit)
                {
                    Functions.Add(MakeShared<FJsonValueObject>(FunctionSummary(Function)));
                    ++Added;
                }
            }
            Obj->SetArrayField(TEXT("functions"), Functions);
            Obj->SetNumberField(TEXT("function_count"), Total);
            Obj->SetBoolField(TEXT("functions_truncated"), Added >= FunctionLimit);
        }

        return Obj;
    }

    static TSharedPtr<FJsonObject> StructSummary(const FCommonStructSpec& Spec, bool bIncludeProperties, int32 PropertyLimit)
    {
        TSharedPtr<FJsonObject> Obj = MakeShared<FJsonObject>();
        Obj->SetStringField(TEXT("name"), Spec.Name);
        Obj->SetStringField(TEXT("path"), Spec.Path);
        Obj->SetStringField(TEXT("plugin"), Spec.Plugin);

        UScriptStruct* Struct = LoadStructPath(Spec.Path);
        Obj->SetBoolField(TEXT("available"), Struct != nullptr);
        if (!Struct)
        {
            return Obj;
        }

        Obj->SetStringField(TEXT("struct_path"), Struct->GetPathName());
        Obj->SetBoolField(TEXT("native"), Struct->StructFlags & STRUCT_Native);
        Obj->SetBoolField(TEXT("blueprint_type"), Struct->HasMetaData(TEXT("BlueprintType")));

        if (bIncludeProperties)
        {
            TArray<TSharedPtr<FJsonValue>> Properties;
            int32 Added = 0;
            int32 Total = 0;
            for (TFieldIterator<FProperty> It(Struct, EFieldIteratorFlags::IncludeSuper); It; ++It)
            {
                ++Total;
                const FProperty* Property = *It;
                if (!Property || !(Property->HasAnyPropertyFlags(CPF_Edit) || Property->HasAnyPropertyFlags(CPF_BlueprintVisible) || Property->HasAnyPropertyFlags(CPF_Config)))
                {
                    continue;
                }
                if (Added < PropertyLimit)
                {
                    Properties.Add(MakeShared<FJsonValueObject>(PropertySummary(Property)));
                    ++Added;
                }
            }
            Obj->SetArrayField(TEXT("properties"), Properties);
            Obj->SetNumberField(TEXT("property_count"), Total);
            Obj->SetBoolField(TEXT("properties_truncated"), Added >= PropertyLimit);
        }

        return Obj;
    }

    static TSharedPtr<FJsonObject> WidgetSummary(UWidget* Widget)
    {
        TSharedPtr<FJsonObject> Obj = MakeShared<FJsonObject>();
        Obj->SetStringField(TEXT("name"), Widget ? Widget->GetName() : FString());
        Obj->SetStringField(TEXT("class"), (Widget && Widget->GetClass()) ? Widget->GetClass()->GetName() : FString());
        Obj->SetStringField(TEXT("class_path"), (Widget && Widget->GetClass()) ? Widget->GetClass()->GetPathName() : FString());
        if (Widget && Widget->Slot)
        {
            Obj->SetStringField(TEXT("slot_class"), Widget->Slot->GetClass()->GetPathName());
        }
        if (Widget)
        {
            if (UPanelWidget* Parent = Widget->GetParent())
            {
                Obj->SetStringField(TEXT("parent"), Parent->GetName());
            }
        }
        return Obj;
    }

    static bool HasGameplayTagProperty(UObject* Object, const TCHAR* PropertyName)
    {
        const FStructProperty* TagProperty = Object ? FindFProperty<FStructProperty>(Object->GetClass(), PropertyName) : nullptr;
        return TagProperty && TagProperty->Struct == FGameplayTag::StaticStruct();
    }

    static FString ReadGameplayTagProperty(UObject* Object, const TCHAR* PropertyName)
    {
        const FStructProperty* TagProperty = Object ? FindFProperty<FStructProperty>(Object->GetClass(), PropertyName) : nullptr;
        if (!TagProperty || TagProperty->Struct != FGameplayTag::StaticStruct())
        {
            return FString();
        }

        const FGameplayTag* TagValue = TagProperty->ContainerPtrToValuePtr<FGameplayTag>(Object);
        return TagValue ? TagValue->ToString() : FString();
    }

    static TSharedPtr<FJsonObject> ExtensionPointSummary(UWidget* Widget)
    {
        TSharedPtr<FJsonObject> Obj = WidgetSummary(Widget);
        Obj->SetStringField(TEXT("extension_point_tag"), ReadGameplayTagProperty(Widget, TEXT("ExtensionPointTag")));

        if (const FProperty* MatchProperty = Widget ? FindFProperty<FProperty>(Widget->GetClass(), TEXT("ExtensionPointTagMatch")) : nullptr)
        {
            Obj->SetStringField(TEXT("extension_point_tag_match"), ExportPropertyValue(MatchProperty, Widget));
        }

        if (const FProperty* DataClassesProperty = Widget ? FindFProperty<FProperty>(Widget->GetClass(), TEXT("DataClasses")) : nullptr)
        {
            Obj->SetStringField(TEXT("data_classes"), ExportPropertyValue(DataClassesProperty, Widget));
        }

        return Obj;
    }

    static void StripWrappingQuotes(FString& Value)
    {
        Value.TrimStartAndEndInline();
        if (Value.Len() >= 2)
        {
            const TCHAR First = Value[0];
            const TCHAR Last = Value[Value.Len() - 1];
            if ((First == TCHAR('"') && Last == TCHAR('"')) || (First == TCHAR('\'') && Last == TCHAR('\'')))
            {
                Value = Value.Mid(1, Value.Len() - 2);
                Value.TrimStartAndEndInline();
            }
        }
    }

    static FString NormalizeClassPath(FString RawValue)
    {
        RawValue.TrimStartAndEndInline();
        StripWrappingQuotes(RawValue);

        if (RawValue.IsEmpty() || RawValue.Equals(TEXT("None"), ESearchCase::IgnoreCase))
        {
            return FString();
        }

        int32 FirstQuote = INDEX_NONE;
        int32 LastQuote = INDEX_NONE;
        if (RawValue.FindChar(TCHAR('\''), FirstQuote) && RawValue.FindLastChar(TCHAR('\''), LastQuote) && LastQuote > FirstQuote)
        {
            RawValue = RawValue.Mid(FirstQuote + 1, LastQuote - FirstQuote - 1);
            RawValue.TrimStartAndEndInline();
            return RawValue;
        }

        const FString AssetPathToken(TEXT("AssetPath="));
        const int32 AssetPathIndex = RawValue.Find(AssetPathToken, ESearchCase::IgnoreCase);
        if (AssetPathIndex != INDEX_NONE)
        {
            FString Remainder = RawValue.Mid(AssetPathIndex + AssetPathToken.Len());
            Remainder.TrimStartAndEndInline();
            StripWrappingQuotes(Remainder);

            int32 EndIndex = INDEX_NONE;
            if (Remainder.FindChar(TCHAR(','), EndIndex) || Remainder.FindChar(TCHAR(')'), EndIndex))
            {
                Remainder = Remainder.Left(EndIndex);
                Remainder.TrimStartAndEndInline();
                StripWrappingQuotes(Remainder);
            }
            return Remainder;
        }

        return RawValue;
    }

    static UClass* LoadObjectClassPath(const FString& RawClassPath)
    {
        const FString ClassPathValue = NormalizeClassPath(RawClassPath);
        return ClassPathValue.IsEmpty() ? nullptr : StaticLoadClass(UObject::StaticClass(), nullptr, *ClassPathValue);
    }

    static FString GetConfigStringValue(const FString& SectionName, const TCHAR* KeyName)
    {
        if (!GConfig || SectionName.IsEmpty())
        {
            return FString();
        }

        FString Value;
        if (GConfig->GetString(*SectionName, KeyName, Value, GGameIni))
        {
            return NormalizeClassPath(Value);
        }
        return FString();
    }

    static FString GetClassDefaultPropertyValue(UClass* Class, const TCHAR* PropertyName)
    {
        if (!Class)
        {
            return FString();
        }

        UObject* DefaultObject = Class->GetDefaultObject();
        const FProperty* Property = FindFProperty<FProperty>(Class, PropertyName);
        return NormalizeClassPath(ExportPropertyValue(Property, DefaultObject));
    }

    static UClass* ResolveMessagingClass(const TSharedPtr<FJsonObject>& Params, UClass* MessagingBaseClass, FString& OutRequestedPath)
    {
        OutRequestedPath = MonolithUIInternal::GetOptionalString(Params, TEXT("messaging_class"));
        if (!OutRequestedPath.IsEmpty())
        {
            return LoadObjectClassPath(OutRequestedPath);
        }

        UClass* LyraMessagingClass = LoadObjectClassPath(TEXT("/Script/LyraGame.LyraUIMessaging"));
        if (LyraMessagingClass && (!MessagingBaseClass || LyraMessagingClass->IsChildOf(MessagingBaseClass)))
        {
            OutRequestedPath = TEXT("/Script/LyraGame.LyraUIMessaging");
            return LyraMessagingClass;
        }

        if (MessagingBaseClass)
        {
            for (TObjectIterator<UClass> It; It; ++It)
            {
                UClass* Candidate = *It;
                if (!Candidate || Candidate == MessagingBaseClass || Candidate->HasAnyClassFlags(CLASS_Abstract | CLASS_Deprecated))
                {
                    continue;
                }
                if (Candidate->IsChildOf(MessagingBaseClass)
                    && FindFProperty<FProperty>(Candidate, TEXT("ConfirmationDialogClass"))
                    && FindFProperty<FProperty>(Candidate, TEXT("ErrorDialogClass")))
                {
                    OutRequestedPath = Candidate->GetPathName();
                    return Candidate;
                }
            }
        }

        OutRequestedPath = MessagingBaseClass ? MessagingBaseClass->GetPathName() : FString();
        return MessagingBaseClass;
    }

    static FString ResolveMessagingConfigSection(const TSharedPtr<FJsonObject>& Params, UClass* MessagingClass)
    {
        const FString ExplicitSection = MonolithUIInternal::GetOptionalString(Params, TEXT("config_section"));
        if (!ExplicitSection.IsEmpty())
        {
            return ExplicitSection;
        }
        return MessagingClass ? MessagingClass->GetPathName() : FString(TEXT("/Script/CommonGame.CommonMessagingSubsystem"));
    }

    static FString ResolveDialogClassPath(
        const TSharedPtr<FJsonObject>& Params,
        const TCHAR* ParamName,
        const FString& ConfigSection,
        const TCHAR* ConfigKey,
        UClass* MessagingClass)
    {
        const FString ExplicitPath = MonolithUIInternal::GetOptionalString(Params, ParamName);
        if (!ExplicitPath.IsEmpty())
        {
            return NormalizeClassPath(ExplicitPath);
        }

        const FString ConfigPath = GetConfigStringValue(ConfigSection, ConfigKey);
        if (!ConfigPath.IsEmpty())
        {
            return ConfigPath;
        }

        return GetClassDefaultPropertyValue(MessagingClass, ConfigKey);
    }

    static TSharedPtr<FJsonObject> MakeCheck(const TCHAR* Name, bool bOk, const TCHAR* Status, const FString& Detail)
    {
        TSharedPtr<FJsonObject> Obj = MakeShared<FJsonObject>();
        Obj->SetStringField(TEXT("name"), Name);
        Obj->SetBoolField(TEXT("ok"), bOk);
        Obj->SetStringField(TEXT("status"), Status);
        Obj->SetStringField(TEXT("detail"), Detail);
        return Obj;
    }

    static void AddCheck(TArray<TSharedPtr<FJsonValue>>& Checks, const TCHAR* Name, bool bOk, const TCHAR* Status, const FString& Detail)
    {
        Checks.Add(MakeShared<FJsonValueObject>(MakeCheck(Name, bOk, Status, Detail)));
    }

    static void AddIssue(TArray<TSharedPtr<FJsonValue>>& Issues, const TCHAR* Code, const FString& Message, const TCHAR* Severity = TEXT("error"))
    {
        TSharedPtr<FJsonObject> Obj = MakeShared<FJsonObject>();
        Obj->SetStringField(TEXT("severity"), Severity);
        Obj->SetStringField(TEXT("code"), Code);
        Obj->SetStringField(TEXT("message"), Message);
        Issues.Add(MakeShared<FJsonValueObject>(Obj));
    }

    static TSharedPtr<FJsonObject> MessagingClassSummary(UClass* MessagingClass, UClass* MessagingBaseClass, const FString& RequestedPath)
    {
        TSharedPtr<FJsonObject> Obj = MakeShared<FJsonObject>();
        Obj->SetStringField(TEXT("requested_class_path"), RequestedPath);
        Obj->SetBoolField(TEXT("found"), MessagingClass != nullptr);
        Obj->SetBoolField(TEXT("base_available"), MessagingBaseClass != nullptr);
        Obj->SetStringField(TEXT("base_class_path"), ClassPath(MessagingBaseClass));
        Obj->SetStringField(TEXT("class_path"), ClassPath(MessagingClass));
        Obj->SetBoolField(TEXT("child_of_common_messaging_subsystem"), MessagingClass && MessagingBaseClass && MessagingClass->IsChildOf(MessagingBaseClass));
        Obj->SetBoolField(TEXT("abstract"), MessagingClass && MessagingClass->HasAnyClassFlags(CLASS_Abstract));
        Obj->SetBoolField(TEXT("deprecated"), MessagingClass && MessagingClass->HasAnyClassFlags(CLASS_Deprecated));
        return Obj;
    }

    static TSharedPtr<FJsonObject> DialogClassSummary(const TCHAR* Role, const FString& RawClassPath, UClass* DialogBaseClass)
    {
        const FString NormalizedPath = NormalizeClassPath(RawClassPath);
        UClass* DialogClass = LoadObjectClassPath(NormalizedPath);

        TSharedPtr<FJsonObject> Obj = MakeShared<FJsonObject>();
        Obj->SetStringField(TEXT("role"), Role);
        Obj->SetStringField(TEXT("input_class_path"), RawClassPath);
        Obj->SetStringField(TEXT("normalized_class_path"), NormalizedPath);
        Obj->SetBoolField(TEXT("found"), DialogClass != nullptr);
        Obj->SetBoolField(TEXT("dialog_base_available"), DialogBaseClass != nullptr);
        Obj->SetStringField(TEXT("resolved_class_path"), ClassPath(DialogClass));
        Obj->SetStringField(TEXT("base_class_path"), ClassPath(DialogBaseClass));
        Obj->SetBoolField(TEXT("child_of_common_game_dialog"), DialogClass && DialogBaseClass && DialogClass->IsChildOf(DialogBaseClass));
        Obj->SetBoolField(TEXT("abstract"), DialogClass && DialogClass->HasAnyClassFlags(CLASS_Abstract));
        Obj->SetBoolField(TEXT("deprecated"), DialogClass && DialogClass->HasAnyClassFlags(CLASS_Deprecated));
        Obj->SetBoolField(
            TEXT("valid_for_common_dialog"),
            DialogClass && DialogBaseClass && DialogClass->IsChildOf(DialogBaseClass) && !DialogClass->HasAnyClassFlags(CLASS_Abstract | CLASS_Deprecated));
        Obj->SetStringField(TEXT("setup_dialog_override_status"), TEXT("not_reflected_native_virtual"));
        return Obj;
    }

    static void AddDialogContractIssues(const TCHAR* Role, const TSharedPtr<FJsonObject>& Dialog, TArray<TSharedPtr<FJsonValue>>& Issues)
    {
        FString NormalizedPath;
        Dialog->TryGetStringField(TEXT("normalized_class_path"), NormalizedPath);
        if (NormalizedPath.IsEmpty())
        {
            AddIssue(Issues, TEXT("dialog_class_missing"), FString::Printf(TEXT("%s dialog class is not configured."), Role));
            return;
        }

        bool bValue = false;
        if (!Dialog->TryGetBoolField(TEXT("found"), bValue) || !bValue)
        {
            AddIssue(Issues, TEXT("dialog_class_not_found"), FString::Printf(TEXT("%s dialog class '%s' could not be loaded."), Role, *NormalizedPath));
        }
        if (!Dialog->TryGetBoolField(TEXT("child_of_common_game_dialog"), bValue) || !bValue)
        {
            AddIssue(Issues, TEXT("dialog_class_wrong_parent"), FString::Printf(TEXT("%s dialog class '%s' is not a CommonGameDialog subclass."), Role, *NormalizedPath));
        }
        if (Dialog->TryGetBoolField(TEXT("abstract"), bValue) && bValue)
        {
            AddIssue(Issues, TEXT("dialog_class_abstract"), FString::Printf(TEXT("%s dialog class '%s' is abstract."), Role, *NormalizedPath));
        }
        if (Dialog->TryGetBoolField(TEXT("deprecated"), bValue) && bValue)
        {
            AddIssue(Issues, TEXT("dialog_class_deprecated"), FString::Printf(TEXT("%s dialog class '%s' is deprecated."), Role, *NormalizedPath));
        }
    }

    static TArray<TSharedPtr<FJsonValue>> MessagingSubclassSummaries(UClass* MessagingBaseClass, int32 Limit)
    {
        TArray<TSharedPtr<FJsonValue>> Classes;
        if (!MessagingBaseClass)
        {
            return Classes;
        }

        int32 Added = 0;
        for (TObjectIterator<UClass> It; It && Added < Limit; ++It)
        {
            UClass* Class = *It;
            if (!Class || !Class->IsChildOf(MessagingBaseClass))
            {
                continue;
            }

            TSharedPtr<FJsonObject> Obj = MakeShared<FJsonObject>();
            Obj->SetStringField(TEXT("class_path"), Class->GetPathName());
            Obj->SetBoolField(TEXT("is_base_class"), Class == MessagingBaseClass);
            Obj->SetBoolField(TEXT("abstract"), Class->HasAnyClassFlags(CLASS_Abstract));
            Obj->SetBoolField(TEXT("deprecated"), Class->HasAnyClassFlags(CLASS_Deprecated));
            Obj->SetBoolField(TEXT("has_confirmation_dialog_class_property"), FindFProperty<FProperty>(Class, TEXT("ConfirmationDialogClass")) != nullptr);
            Obj->SetBoolField(TEXT("has_error_dialog_class_property"), FindFProperty<FProperty>(Class, TEXT("ErrorDialogClass")) != nullptr);
            Classes.Add(MakeShared<FJsonValueObject>(Obj));
            ++Added;
        }
        return Classes;
    }

    static FString GetDefaultUIPolicyClassPath()
    {
        return GetConfigStringValue(TEXT("/Script/CommonGame.GameUIManagerSubsystem"), TEXT("DefaultUIPolicyClass"));
    }
}

void FMonolithUIActions::RegisterActions(FMonolithToolRegistry& Registry)
{
    Registry.RegisterAction(
        TEXT("ui"), TEXT("create_widget_blueprint"),
        TEXT("Create a UWidgetBlueprint at save_path. parent_class accepts /Script/Module.Class form OR short name ('TokenforgeActivatableWidget', 'CommonActivatableWidget', 'UserWidget') — short name resolves via UClass::FindClassByName."),
        FMonolithActionHandler::CreateStatic(&HandleCreateWidgetBlueprint),
        FParamSchemaBuilder()
            .RequiredAssetPath(TEXT("save_path"), TEXT("Asset path, e.g. /Game/UI/WBP_MyWidget"))
            .Optional(TEXT("parent_class"), TEXT("string"), TEXT("Parent class name (default: UserWidget)"), TEXT("UserWidget"))
            .Optional(TEXT("root_widget"), TEXT("string"), TEXT("Root widget type (default: CanvasPanel)"), TEXT("CanvasPanel"))
            .Optional(TEXT("skip_save"), TEXT("boolean"), TEXT("Skip saving to disk"), TEXT("false"))
            .Build()
    );

    Registry.RegisterAction(
        TEXT("ui"), TEXT("get_widget_tree"),
        TEXT("Get the full widget hierarchy of a Widget Blueprint as JSON"),
        FMonolithActionHandler::CreateStatic(&HandleGetWidgetTree),
        FParamSchemaBuilder()
            .RequiredAssetPath(TEXT("asset_path"), TEXT("Widget Blueprint asset path"))
            .Build()
    );

    Registry.RegisterAction(
        TEXT("ui"), TEXT("add_widget"),
        TEXT("Add a widget to a parent panel in a Widget Blueprint"),
        FMonolithActionHandler::CreateStatic(&HandleAddWidget),
        FParamSchemaBuilder()
            .RequiredAssetPath(TEXT("asset_path"), TEXT("Widget Blueprint asset path"))
            .Required(TEXT("widget_class"), TEXT("string"), TEXT("Widget class: TextBlock, Image, Button, VerticalBox, etc."))
            .Optional(TEXT("widget_name"), TEXT("string"), TEXT("Name for the new widget (auto-generated if omitted)"))
            .Optional(TEXT("parent_name"), TEXT("string"), TEXT("Parent widget name (default: root widget)"))
            .Optional(TEXT("anchor_preset"), TEXT("string"), TEXT("Anchor preset: center, top_left, stretch_fill, etc."))
            .Optional(TEXT("position"), TEXT("object"), TEXT("Canvas position: {\"x\": 0, \"y\": 0}"))
            .Optional(TEXT("size"), TEXT("object"), TEXT("Canvas size: {\"x\": 200, \"y\": 50}"))
            .Optional(TEXT("padding"), TEXT("object"), TEXT("Slot padding: {\"left\":0,\"top\":0,\"right\":0,\"bottom\":0}"))
            .Optional(TEXT("h_align"), TEXT("string"), TEXT("Horizontal alignment: Left, Center, Right, Fill"))
            .Optional(TEXT("v_align"), TEXT("string"), TEXT("Vertical alignment: Top, Center, Bottom, Fill"))
            .Optional(TEXT("auto_size"), TEXT("boolean"), TEXT("Auto-size in canvas slot"), TEXT("false"))
            .Optional(TEXT("compile"), TEXT("boolean"), TEXT("Compile after adding"), TEXT("true"))
            .Build()
    );

    Registry.RegisterAction(
        TEXT("ui"), TEXT("add_extension_point_widget"),
        TEXT("Add or update a UIExtensionPointWidget-compatible widget in a Widget Blueprint, assign the requested GameplayTag on its ExtensionPointTag property, and attach it to the requested parent with deterministic slot layout. Resolves UIExtensionPointWidget by class path/reflection so MonolithUI does not hard-link the optional UIExtension plugin."),
        FMonolithActionHandler::CreateStatic(&HandleAddExtensionPointWidget),
        FParamSchemaBuilder()
            .RequiredAssetPath(TEXT("asset_path"), TEXT("Widget Blueprint asset path"))
            .Required(TEXT("widget_name"), TEXT("string"), TEXT("Name of the extension point widget to create or update"))
            .Required(TEXT("extension_point_tag"), TEXT("string"), TEXT("Registered GameplayTag to assign to ExtensionPointTag"))
            .Optional(TEXT("widget_class"), TEXT("string"), TEXT("Widget class path or token. Defaults to /Script/UIExtension.UIExtensionPointWidget"), TEXT("/Script/UIExtension.UIExtensionPointWidget"))
            .Optional(TEXT("parent_name"), TEXT("string"), TEXT("Parent panel widget name. Omitted = root panel; if root is empty, a CanvasPanel root is created."))
            .Optional(TEXT("anchor_preset"), TEXT("string"), TEXT("Canvas anchor preset: center, top_left, stretch_fill, etc."))
            .Optional(TEXT("position"), TEXT("object"), TEXT("Canvas position: {\"x\": 0, \"y\": 0}"))
            .Optional(TEXT("size"), TEXT("object"), TEXT("Canvas size: {\"x\": 200, \"y\": 50}"))
            .Optional(TEXT("alignment"), TEXT("object"), TEXT("Canvas alignment: {\"x\": 0.5, \"y\": 0.5}"))
            .Optional(TEXT("padding"), TEXT("object"), TEXT("Slot padding: {\"left\":0,\"top\":0,\"right\":0,\"bottom\":0}"))
            .Optional(TEXT("h_align"), TEXT("string"), TEXT("Horizontal alignment: Left, Center, Right, Fill"))
            .Optional(TEXT("v_align"), TEXT("string"), TEXT("Vertical alignment: Top, Center, Bottom, Fill"))
            .Optional(TEXT("auto_size"), TEXT("boolean"), TEXT("Auto-size in canvas slot"), TEXT("false"))
            .Optional(TEXT("compile"), TEXT("boolean"), TEXT("Compile after a change"), TEXT("true"))
            .Optional(TEXT("save"), TEXT("boolean"), TEXT("Save package after a change"), TEXT("false"))
            .Build()
    );

    Registry.RegisterAction(
        TEXT("ui"), TEXT("get_common_framework_status"),
        TEXT("Describe optional Lyra Common framework availability for CommonUI, CommonGame, UIExtension, CommonUser, CommonLoadingScreen, GameSettings, GameplayMessageRouter, ModularGameplayActors, and GameSubtitles without hard-linking those plugins."),
        FMonolithActionHandler::CreateStatic(&HandleGetCommonFrameworkStatus),
        FParamSchemaBuilder()
            .Optional(TEXT("include_properties"), TEXT("boolean"), TEXT("Include reflected class properties"), TEXT("false"))
            .Optional(TEXT("include_functions"), TEXT("boolean"), TEXT("Include reflected class functions"), TEXT("false"))
            .Optional(TEXT("property_limit"), TEXT("integer"), TEXT("Maximum properties per class"), TEXT("40"))
            .Optional(TEXT("function_limit"), TEXT("integer"), TEXT("Maximum functions per class"), TEXT("80"))
            .Build(),
        TEXT("CommonFramework")
    );

    Registry.RegisterAction(
        TEXT("ui"), TEXT("add_primary_game_layout_layer"),
        TEXT("Add or update a CommonActivatableWidgetContainerBase-compatible layer widget inside a PrimaryGameLayout Widget Blueprint. The action edits only the Widget Blueprint tree and returns the RegisterLayer tag/widget pair that the layout graph should use."),
        FMonolithActionHandler::CreateStatic(&HandleAddPrimaryGameLayoutLayer),
        FParamSchemaBuilder()
            .RequiredAssetPath(TEXT("asset_path"), TEXT("PrimaryGameLayout Widget Blueprint asset path"))
            .Required(TEXT("layer_tag"), TEXT("string"), TEXT("Registered UI.Layer GameplayTag for the layer"))
            .Optional(TEXT("widget_name"), TEXT("string"), TEXT("Name for the layer container widget. Defaults to <tag leaf>Stack"))
            .Optional(TEXT("widget_class"), TEXT("string"), TEXT("CommonActivatableWidgetContainerBase subclass. Defaults to /Script/CommonUI.CommonActivatableWidgetStack"), TEXT("/Script/CommonUI.CommonActivatableWidgetStack"))
            .Optional(TEXT("parent_name"), TEXT("string"), TEXT("Parent panel widget name. Omitted = root panel; if root is empty, a CanvasPanel root is created."))
            .Optional(TEXT("anchor_preset"), TEXT("string"), TEXT("Canvas anchor preset: center, top_left, stretch_fill, etc."))
            .Optional(TEXT("position"), TEXT("object"), TEXT("Canvas position: {\"x\": 0, \"y\": 0}"))
            .Optional(TEXT("size"), TEXT("object"), TEXT("Canvas size: {\"x\": 200, \"y\": 50}"))
            .Optional(TEXT("alignment"), TEXT("object"), TEXT("Canvas alignment: {\"x\": 0.5, \"y\": 0.5}"))
            .Optional(TEXT("padding"), TEXT("object"), TEXT("Slot padding: {\"left\":0,\"top\":0,\"right\":0,\"bottom\":0}"))
            .Optional(TEXT("h_align"), TEXT("string"), TEXT("Horizontal alignment: Left, Center, Right, Fill"))
            .Optional(TEXT("v_align"), TEXT("string"), TEXT("Vertical alignment: Top, Center, Bottom, Fill"))
            .Optional(TEXT("auto_size"), TEXT("boolean"), TEXT("Auto-size in canvas slot"), TEXT("false"))
            .Optional(TEXT("compile"), TEXT("boolean"), TEXT("Compile after a change"), TEXT("true"))
            .Optional(TEXT("save"), TEXT("boolean"), TEXT("Save package after a change"), TEXT("false"))
            .Build(),
        TEXT("CommonFramework")
    );

    Registry.RegisterAction(
        TEXT("ui"), TEXT("describe_common_widget_blueprint"),
        TEXT("Inspect a Widget Blueprint for Lyra Common UI semantics: PrimaryGameLayout parentage, UIExtensionPointWidget tags, and CommonActivatableWidgetContainerBase layer candidates."),
        FMonolithActionHandler::CreateStatic(&HandleDescribeCommonWidgetBlueprint),
        FParamSchemaBuilder()
            .RequiredAssetPath(TEXT("asset_path"), TEXT("Widget Blueprint asset path"))
            .Optional(TEXT("include_extension_points"), TEXT("boolean"), TEXT("List UIExtensionPointWidget-compatible widgets"), TEXT("true"))
            .Optional(TEXT("include_layer_candidates"), TEXT("boolean"), TEXT("List CommonActivatableWidgetContainerBase-compatible widgets"), TEXT("true"))
            .Optional(TEXT("include_widget_tree"), TEXT("boolean"), TEXT("Include a flat summary of every widget in the tree"), TEXT("false"))
            .Build(),
        TEXT("CommonFramework")
    );

    Registry.RegisterAction(
        TEXT("ui"), TEXT("describe_common_messaging_flow"),
        TEXT("Describe the CommonGame messaging flow without runtime edits: CommonMessagingSubsystem class/config, CommonGame dialog classes, modal layer tag, DefaultUIPolicyClass, and reflected messaging subclasses."),
        FMonolithActionHandler::CreateStatic(&HandleDescribeCommonMessagingFlow),
        FParamSchemaBuilder()
            .Optional(TEXT("messaging_class"), TEXT("string"), TEXT("CommonMessagingSubsystem subclass path. Defaults to the detected project subclass, then CommonMessagingSubsystem."))
            .Optional(TEXT("config_section"), TEXT("string"), TEXT("Config section for dialog class properties. Defaults to the selected messaging class path."))
            .Optional(TEXT("modal_layer_tag"), TEXT("string"), TEXT("Modal layer GameplayTag to inspect."), TEXT("UI.Layer.Modal"))
            .Optional(TEXT("include_subclasses"), TEXT("boolean"), TEXT("Include loaded CommonMessagingSubsystem subclasses."), TEXT("true"))
            .Optional(TEXT("subclass_limit"), TEXT("integer"), TEXT("Maximum loaded messaging subclasses to report."), TEXT("40"))
            .Build(),
        TEXT("CommonFramework")
    );

    Registry.RegisterAction(
        TEXT("ui"), TEXT("validate_common_dialog_contract"),
        TEXT("Validate CommonGame dialog class configuration for a CommonMessagingSubsystem subclass. Reads config or explicit dialog class paths and checks CommonGameDialog compatibility."),
        FMonolithActionHandler::CreateStatic(&HandleValidateCommonDialogContract),
        FParamSchemaBuilder()
            .Optional(TEXT("messaging_class"), TEXT("string"), TEXT("CommonMessagingSubsystem subclass path. Defaults to the detected project subclass, then CommonMessagingSubsystem."))
            .Optional(TEXT("config_section"), TEXT("string"), TEXT("Config section for ConfirmationDialogClass and ErrorDialogClass. Defaults to the selected messaging class path."))
            .Optional(TEXT("confirmation_dialog_class"), TEXT("string"), TEXT("Explicit confirmation dialog class path. Overrides config."))
            .Optional(TEXT("error_dialog_class"), TEXT("string"), TEXT("Explicit error dialog class path. Overrides config."))
            .Build(),
        TEXT("CommonFramework")
    );

    Registry.RegisterAction(
        TEXT("ui"), TEXT("validate_common_layer_push_contract"),
        TEXT("Validate the CommonGame modal layer push contract without changing PrimaryGameLayout: tag registration, optional layout WBP layer candidates, dialog class compatibility, and RegisterLayer evidence limits."),
        FMonolithActionHandler::CreateStatic(&HandleValidateCommonLayerPushContract),
        FParamSchemaBuilder()
            .Optional(TEXT("layout_asset_path"), TEXT("string"), TEXT("PrimaryGameLayout Widget Blueprint asset path to inspect."))
            .Optional(TEXT("layer_tag"), TEXT("string"), TEXT("GameplayTag used for PushWidgetToLayerStack."), TEXT("UI.Layer.Modal"))
            .Optional(TEXT("layer_widget_name"), TEXT("string"), TEXT("Expected CommonActivatableWidgetContainerBase widget name inside the layout WBP."))
            .Optional(TEXT("dialog_class"), TEXT("string"), TEXT("Dialog class path to validate against CommonGameDialog."))
            .Optional(TEXT("require_layout_asset"), TEXT("boolean"), TEXT("Reject calls without layout_asset_path."), TEXT("false"))
            .Build(),
        TEXT("CommonFramework")
    );

    Registry.RegisterAction(
        TEXT("ui"), TEXT("validate_frontend_menu_flow"),
        TEXT("Validate a Lyra/CommonUI front-end menu flow without runtime edits: PrimaryGameLayout layer candidates, dialog push contract, activatable screen widgets, expected/forbidden widgets, variable defaults, and optional graph text evidence."),
        FMonolithActionHandler::CreateStatic(&HandleValidateFrontendMenuFlow),
        FParamSchemaBuilder()
            .Optional(TEXT("layout_asset_path"), TEXT("string"), TEXT("PrimaryGameLayout Widget Blueprint asset path to inspect."))
            .Optional(TEXT("required_layers"), TEXT("array"), TEXT("Required layer specs as strings or {layer_tag, widget_name} objects."))
            .Optional(TEXT("screens"), TEXT("array"), TEXT("Screen specs: {asset_path, role, require_common_activatable, expected_parent_class, required_widgets[], forbidden_widgets[], expected_variables{}, desired_focus_widget, required_graph_needles[], forbidden_graph_needles[]}."))
            .Optional(TEXT("modal_layer_tag"), TEXT("string"), TEXT("Modal layer GameplayTag used by dialog pushes."), TEXT("UI.Layer.Modal"))
            .Optional(TEXT("dialog_class"), TEXT("string"), TEXT("Dialog class path to validate against CommonGameDialog."))
            .Optional(TEXT("require_layout_asset"), TEXT("boolean"), TEXT("Fail when layout_asset_path is omitted."), TEXT("false"))
            .Optional(TEXT("require_dialog"), TEXT("boolean"), TEXT("Fail when no dialog class is supplied or configured."), TEXT("false"))
            .Optional(TEXT("include_graph_scan"), TEXT("boolean"), TEXT("Scan Blueprint graph node titles and pin defaults for required/forbidden graph needles."), TEXT("true"))
            .Build(),
        TEXT("CommonFramework")
    );

    Registry.RegisterAction(
        TEXT("ui"), TEXT("remove_widget"),
        TEXT("Remove a widget from a Widget Blueprint"),
        FMonolithActionHandler::CreateStatic(&HandleRemoveWidget),
        FParamSchemaBuilder()
            .RequiredAssetPath(TEXT("asset_path"), TEXT("Widget Blueprint asset path"))
            .Required(TEXT("widget_name"), TEXT("string"), TEXT("Name of the widget to remove"))
            .Optional(TEXT("compile"), TEXT("boolean"), TEXT("Compile after removing"), TEXT("true"))
            .Build()
    );

    Registry.RegisterAction(
        TEXT("ui"), TEXT("set_widget_property"),
        TEXT("Set a property on a widget (text, color, opacity, visibility, etc.). Default mode gates writes through the per-type curated allowlist; pass raw_mode=true to bypass the gate (legacy compat). The new value can be supplied as `value` OR the alias `property_value` (Bug #6 fix). `IsVariable`/`bIsVariable` routes to the first-class variable-flag path."),
        FMonolithActionHandler::CreateStatic(&HandleSetWidgetProperty),
        FParamSchemaBuilder()
            .RequiredAssetPath(TEXT("asset_path"), TEXT("Widget Blueprint asset path"))
            .Required(TEXT("widget_name"), TEXT("string"), TEXT("Target widget name"))
            .Required(TEXT("property_name"), TEXT("string"), TEXT("Property path. Dotted segments allowed (e.g. 'Padding.Left'). Allowlist-gated unless raw_mode=true. `IsVariable`/`bIsVariable` is accepted as a compatibility route to set_widget_is_variable."))
            .Required(TEXT("value"), TEXT("string"), TEXT("Property value (alias: 'property_value'). Strings, numbers, booleans, JSON arrays/objects all accepted; struct types (Vector2D/LinearColor/Margin/Vector4/SlateColor) accept multiple shapes."))
            .Optional(TEXT("compile"), TEXT("boolean"), TEXT("Compile after setting"), TEXT("false"))
            .Optional(TEXT("raw_mode"), TEXT("boolean"), TEXT("Bypass the allowlist gate (legacy unconditional ImportText_Direct path). Default false."), TEXT("false"))
            .Build()
    );

    Registry.RegisterAction(
        TEXT("ui"), TEXT("compile_widget"),
        TEXT("Compile a Widget Blueprint. Response includes errors[] and warnings[] arrays populated when status=BS_Error (added v0.14.11)."),
        FMonolithActionHandler::CreateStatic(&HandleCompileWidget),
        FParamSchemaBuilder()
            .RequiredAssetPath(TEXT("asset_path"), TEXT("Widget Blueprint asset path"))
            .Build()
    );

    Registry.RegisterAction(
        TEXT("ui"), TEXT("list_widget_types"),
        TEXT("List all available widget class types that can be added"),
        FMonolithActionHandler::CreateStatic(&HandleListWidgetTypes),
        FParamSchemaBuilder()
            .Optional(TEXT("filter"), TEXT("string"), TEXT("Filter by category: panel, leaf, input, display, layout"))
            .Build()
    );

    // Phase 2 Item #7 (2026-05-16 UI gap audit): rename a widget in-place.
    // Recompiles via FKismetEditorUtilities::CompileBlueprint so the structural
    // modification + the Skeleton class refresh + the post-compile reflection
    // walk all see the new FName.
    Registry.RegisterAction(
        TEXT("ui"), TEXT("rename_widget"),
        TEXT("Rename a UWidget's FName in a WBP's tree; updates slot references and recompiles. "
             "Uniqueness check runs against the full WidgetTree before the rename — colliding new_name returns -32602."),
        FMonolithActionHandler::CreateStatic(&MonolithUIActionsPhase2::HandleRenameWidget),
        FParamSchemaBuilder()
            .RequiredAssetPath(TEXT("wbp_path"), TEXT("Widget Blueprint path (alias: asset_path)"), {TEXT("asset_path")})
            .Required(TEXT("old_name"), TEXT("string"), TEXT("Current widget FName"))
            .Required(TEXT("new_name"), TEXT("string"), TEXT("Target widget FName (must be unique in tree)"))
            .Build(),
        TEXT("WidgetCRUD")
    );

    // Phase 2 Item #14 (2026-05-16 UI gap audit): re-compile a blueprint and
    // return the last_compile_status (EBlueprintStatus → string) + the
    // FCompilerResultsLog errors[]/warnings[]/notes[]. Phase 1's HandleCompileWidget
    // captures the log on every call but does not cache it on the asset — so a
    // dump call drives a fresh compile to harvest the messages. Shape mirrors
    // blueprint_query("compile_blueprint") for parser reuse.
    Registry.RegisterAction(
        TEXT("ui"), TEXT("dump_blueprint_compile_log"),
        TEXT("Run a fresh compile and return last_compile_status + errors[]/warnings[]/notes[]. "
             "Same shape as compile_widget on success; useful when a prior call did not retain its log "
             "(orchestrator did not parse the response, retried later, etc.). Accepts UWidgetBlueprint OR "
             "UBlueprint paths — the action sniffs the type and reads ::Status accordingly."),
        FMonolithActionHandler::CreateStatic(&MonolithUIActionsPhase2::HandleDumpBlueprintCompileLog),
        FParamSchemaBuilder()
            .RequiredAssetPath(TEXT("asset_path"), TEXT("Blueprint or Widget Blueprint asset path"))
            .Build(),
        TEXT("WidgetCRUD")
    );

	FMonolithToolRegistry::Get().SetActionSearchMetadata(TEXT("ui"), TEXT("create_widget_blueprint"),
		{ TEXT("new WBP"), TEXT("make HUD widget"), TEXT("UMG widget blueprint"), TEXT("UserWidget asset"), TEXT("menu screen") },
		{ TEXT("create_widget"), TEXT("new_widget_blueprint"), TEXT("make_wbp"), TEXT("create_umg") },
		{ TEXT("create a WBP_HUD widget blueprint under /Game/UI"), TEXT("make a new UMG menu widget from CommonActivatableWidget") });

	FMonolithToolRegistry::Get().SetActionSearchMetadata(TEXT("ui"), TEXT("get_widget_tree"),
		{ TEXT("widget hierarchy"), TEXT("list widgets in WBP"), TEXT("inspect UMG layout"), TEXT("child widgets"), TEXT("widget names") },
		{ TEXT("dump_widget_tree"), TEXT("read_widget_hierarchy"), TEXT("get_widgets"), TEXT("show_widget_tree") },
		{ TEXT("show the widget hierarchy of WBP_HUD"), TEXT("what widgets are inside this UMG blueprint") });

	FMonolithToolRegistry::Get().SetActionSearchMetadata(TEXT("ui"), TEXT("add_widget"),
		{ TEXT("insert widget"), TEXT("add button to panel"), TEXT("place TextBlock"), TEXT("new child widget"), TEXT("add image to canvas") },
		{ TEXT("create_widget_element"), TEXT("add_child_widget"), TEXT("insert_widget"), TEXT("add_umg_element") },
		{ TEXT("add a Button named PlayButton to the root canvas of WBP_Menu"), TEXT("put a TextBlock inside the VerticalBox") });

	FMonolithToolRegistry::Get().SetActionSearchMetadata(TEXT("ui"), TEXT("set_widget_property"),
		{ TEXT("set widget text"), TEXT("change widget color"), TEXT("edit widget opacity"), TEXT("widget visibility"), TEXT("mark as variable") },
		{ TEXT("edit_widget_property"), TEXT("update_widget"), TEXT("set_text"), TEXT("configure_widget") },
		{ TEXT("set the Text of TitleLabel to 'Start Game'"), TEXT("change the HealthBar fill color in WBP_HUD") });

	FMonolithToolRegistry::Get().SetActionSearchMetadata(TEXT("ui"), TEXT("get_common_framework_status"),
		{ TEXT("CommonGame"), TEXT("CommonUser"), TEXT("UIExtension"), TEXT("CommonLoadingScreen"), TEXT("GameSettings"), TEXT("GameplayMessageRouter"), TEXT("ModularGameplayActors"), TEXT("GameSubtitles"), TEXT("PrimaryGameLayout"), TEXT("GameUIPolicy") },
		{ TEXT("common framework status"), TEXT("lyra common ui status"), TEXT("common plugin diagnostics"), TEXT("loading screen settings"), TEXT("game settings registry"), TEXT("gameplay message subsystem") },
		{ TEXT("check whether CommonGame and UIExtension are available"), TEXT("list reflected PrimaryGameLayout and GameUIPolicy properties"), TEXT("report CommonLoadingScreen GameSettings GameplayMessageRouter ModularGameplayActors availability") });

	FMonolithToolRegistry::Get().SetActionSearchMetadata(TEXT("ui"), TEXT("describe_common_widget_blueprint"),
		{ TEXT("PrimaryGameLayout"), TEXT("UIExtensionPointWidget"), TEXT("ExtensionPointTag"), TEXT("CommonActivatableWidgetContainerBase"), TEXT("UI layer") },
		{ TEXT("inspect common WBP"), TEXT("describe primary game layout"), TEXT("list extension points"), TEXT("find UI layers") },
		{ TEXT("describe CommonGame layer widgets in WBP_PrimaryGameLayout"), TEXT("list UIExtension point tags in this widget blueprint") });

	FMonolithToolRegistry::Get().SetActionSearchMetadata(TEXT("ui"), TEXT("validate_frontend_menu_flow"),
		{ TEXT("Lyra front-end menu validation"), TEXT("CommonUI activatable screens"), TEXT("PrimaryGameLayout layer candidates"), TEXT("front-end flow widget contract"), TEXT("menu graph needle validation") },
		{ TEXT("validate_menu_flow"), TEXT("validate_common_frontend"), TEXT("validate_frontend_widgets") },
		{ TEXT("validate copied ExperienceSelection and HostSession screens after a package graph copy"), TEXT("check required widgets, forbidden widgets, variable defaults, and layout layers without editing assets") });

	FMonolithToolRegistry::Get().SetActionSearchMetadata(TEXT("ui"), TEXT("compile_widget"),
		{ TEXT("compile WBP"), TEXT("build widget blueprint"), TEXT("check widget compile errors"), TEXT("recompile UMG"), TEXT("validate widget") },
		{ TEXT("compile_widget_blueprint"), TEXT("build_wbp"), TEXT("recompile_widget"), TEXT("compile_umg") },
		{ TEXT("compile WBP_HUD and report any errors"), TEXT("recompile the menu widget after editing it") });
}

// --- create_widget_blueprint ---
FMonolithActionResult FMonolithUIActions::HandleCreateWidgetBlueprint(const TSharedPtr<FJsonObject>& Params)
{
    FString SavePath;
    if (!Params.IsValid() || !Params->TryGetStringField(TEXT("save_path"), SavePath))
    {
        return FMonolithActionResult::Error(TEXT("Missing required param: save_path"));
    }

    // Defensive: reject malformed paths (e.g. "//Game/...") before they reach CreatePackage,
    // which asserts in UObjectGlobals.cpp and kills the editor.
    if (const FString ValidationError = MonolithCore::ValidatePackagePath(SavePath); !ValidationError.IsEmpty())
    {
        return FMonolithActionResult::Error(ValidationError);
    }

    FString ParentClassName = MonolithUIInternal::GetOptionalString(Params, TEXT("parent_class"));
    if (ParentClassName.IsEmpty()) ParentClassName = TEXT("UserWidget");

    FString RootWidgetType = MonolithUIInternal::GetOptionalString(Params, TEXT("root_widget"));
    if (RootWidgetType.IsEmpty()) RootWidgetType = TEXT("CanvasPanel");

    const bool bSkipSave = MonolithUIInternal::GetOptionalBool(Params, TEXT("skip_save"), false);

    // Resolve parent class
    UClass* ParentClass = FindFirstObject<UClass>(*ParentClassName, EFindFirstObjectOptions::NativeFirst);
    if (!ParentClass)
    {
        ParentClass = FindFirstObject<UClass>(*(TEXT("U") + ParentClassName), EFindFirstObjectOptions::NativeFirst);
    }
    if (!ParentClass || !ParentClass->IsChildOf(UUserWidget::StaticClass()))
    {
        // Phase K — surface the failure in FUISpecError shape so the LLM gets
        // category/json_path/suggested_fix/valid_options fields. The valid_options
        // list intentionally enumerates only the common parent classes (the
        // FindFirstObject path supports any UUserWidget subclass — listing
        // every BP-derived UserWidget would explode).
        return MonolithUIInternal::MakeErrorFromSpecError(MonolithUIInternal::MakeSpecError(
            TEXT("Type"),
            TEXT("/parent_class"),
            FString::Printf(TEXT("Parent class '%s' not found or not a UUserWidget subclass."), *ParentClassName),
            TEXT("Use a token (UserWidget / CommonActivatableWidget / CommonUserWidget) or a full /Script/Module.Class path."),
            { TEXT("UserWidget"), TEXT("CommonActivatableWidget"), TEXT("CommonUserWidget") }));
    }

    // Create package
    FString PackagePath, AssetName;
    SavePath.Split(TEXT("/"), &PackagePath, &AssetName, ESearchCase::IgnoreCase, ESearchDir::FromEnd);
    if (AssetName.IsEmpty())
    {
        return FMonolithActionResult::Error(TEXT("Invalid save_path — must contain at least one / separator"));
    }

    UPackage* Package = CreatePackage(*SavePath);
    if (!Package)
    {
        // Phase K — internal error (-32603), not invalid-params: the path passed
        // earlier validation but the engine refused. Caller can't fix this from
        // their end without changing the path.
        return MonolithUIInternal::MakeErrorFromSpecError(MonolithUIInternal::MakeSpecError(
            TEXT("AssetCreate"),
            TEXT("/save_path"),
            FString::Printf(TEXT("CreatePackage failed for '%s'."), *SavePath),
            TEXT("Verify the path is writeable and not in use by the editor.")), -32603);
    }

    // Fail cleanly if the asset already exists instead of letting FactoryCreateNew assert.
    if (FindObject<UObject>(Package, *AssetName))
    {
        return MonolithUIInternal::MakeErrorFromSpecError(MonolithUIInternal::MakeSpecError(
            TEXT("AssetExists"),
            TEXT("/save_path"),
            FString::Printf(TEXT("Widget Blueprint already exists at '%s'."), *SavePath),
            TEXT("Pick a different save_path, or delete the existing asset first.")));
    }

    const FString ObjectPath = FString::Printf(TEXT("%s.%s"), *SavePath, *AssetName);
    FAssetRegistryModule& AssetRegistryModule = FModuleManager::LoadModuleChecked<FAssetRegistryModule>(TEXT("AssetRegistry"));
    if (AssetRegistryModule.Get().GetAssetByObjectPath(FSoftObjectPath(ObjectPath)).IsValid())
    {
        return MonolithUIInternal::MakeErrorFromSpecError(MonolithUIInternal::MakeSpecError(
            TEXT("AssetExists"),
            TEXT("/save_path"),
            FString::Printf(TEXT("Widget Blueprint already exists at '%s' (asset registry)."), *SavePath),
            TEXT("Pick a different save_path, or delete the existing asset first.")));
    }

    // Create widget blueprint via factory
    UWidgetBlueprintFactory* Factory = NewObject<UWidgetBlueprintFactory>();
    Factory->BlueprintType = BPTYPE_Normal;
    Factory->ParentClass = ParentClass;

    UObject* CreatedObj = Factory->FactoryCreateNew(
        UWidgetBlueprint::StaticClass(), Package,
        FName(*AssetName), RF_Public | RF_Standalone,
        nullptr, GWarn);

    UWidgetBlueprint* WBP = Cast<UWidgetBlueprint>(CreatedObj);
    if (!WBP)
    {
        return FMonolithActionResult::Error(TEXT("UWidgetBlueprintFactory::FactoryCreateNew returned null"));
    }

    // Set root widget if tree is empty
    if (WBP->WidgetTree && !WBP->WidgetTree->RootWidget)
    {
        UClass* RootClass = MonolithUIInternal::WidgetClassFromName(RootWidgetType);
        if (RootClass && RootClass->IsChildOf(UPanelWidget::StaticClass()))
        {
            UWidget* Root = WBP->WidgetTree->ConstructWidget<UWidget>(RootClass, FName(*RootWidgetType));
            WBP->WidgetTree->RootWidget = Root;
            MonolithUIInternal::RegisterCreatedWidget(WBP, Root);
        }
    }

    // Compile
    MonolithUIInternal::ReconcileWidgetVariableGuids(WBP);
    FBlueprintEditorUtils::MarkBlueprintAsStructurallyModified(WBP);
    FKismetEditorUtilities::CompileBlueprint(WBP);

    // Save
    if (!bSkipSave)
    {
        FAssetRegistryModule::AssetCreated(WBP);
        Package->MarkPackageDirty();
        FSavePackageArgs SaveArgs;
        SaveArgs.TopLevelFlags = RF_Public | RF_Standalone;
        UPackage::SavePackage(Package, WBP,
            *FPackageName::LongPackageNameToFilename(SavePath, FPackageName::GetAssetPackageExtension()),
            SaveArgs);
    }

    // Build result
    TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
    Result->SetStringField(TEXT("asset_path"), SavePath);
    Result->SetStringField(TEXT("asset_name"), AssetName);
    Result->SetStringField(TEXT("parent_class"), ParentClass->GetName());
    Result->SetStringField(TEXT("root_widget"), RootWidgetType);
    Result->SetBoolField(TEXT("compiled"), true);
    Result->SetBoolField(TEXT("saved"), !bSkipSave);

    return FMonolithActionResult::Success(Result);
}

// --- get_widget_tree ---
FMonolithActionResult FMonolithUIActions::HandleGetWidgetTree(const TSharedPtr<FJsonObject>& Params)
{
    FMonolithActionResult ParamError;
    FString AssetPath;
    if (!MonolithUIInternal::TryGetRequiredString(Params, TEXT("asset_path"), AssetPath, ParamError))
        return ParamError;

    FMonolithActionResult Err;
    UWidgetBlueprint* WBP = MonolithUIInternal::LoadWidgetBlueprint(AssetPath, Err);
    if (!WBP) return Err;

    if (!WBP->WidgetTree)
    {
        return FMonolithActionResult::Error(TEXT("WidgetTree is null (editor-only data not available)"));
    }

    TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
    Result->SetStringField(TEXT("asset_path"), AssetPath);
    Result->SetStringField(TEXT("parent_class"), WBP->ParentClass ? WBP->ParentClass->GetName() : TEXT("None"));

    // Serialize root
    if (WBP->WidgetTree->RootWidget)
    {
        Result->SetObjectField(TEXT("root"), MonolithUIInternal::SerializeWidget(WBP->WidgetTree->RootWidget));
    }

    // Widget count
    TArray<UWidget*> AllWidgets;
    WBP->WidgetTree->GetAllWidgets(AllWidgets);
    Result->SetNumberField(TEXT("widget_count"), AllWidgets.Num());

    // Animations
    TArray<TSharedPtr<FJsonValue>> AnimArray;
    AnimArray.Reserve(WBP->Animations.Num());
    for (UWidgetAnimation* Anim : WBP->Animations)
    {
        if (Anim)
        {
            TSharedPtr<FJsonObject> AnimObj = MakeShared<FJsonObject>();
            AnimObj->SetStringField(TEXT("name"), Anim->GetName());
            AnimObj->SetNumberField(TEXT("start_time"), Anim->GetStartTime());
            AnimObj->SetNumberField(TEXT("end_time"), Anim->GetEndTime());
            AnimArray.Add(MakeShared<FJsonValueObject>(AnimObj));
        }
    }
    if (AnimArray.Num() > 0)
    {
        Result->SetArrayField(TEXT("animations"), AnimArray);
    }

    return FMonolithActionResult::Success(Result);
}

// --- add_widget ---
FMonolithActionResult FMonolithUIActions::HandleAddWidget(const TSharedPtr<FJsonObject>& Params)
{
    FMonolithActionResult ParamError;
    FString AssetPath;
    if (!MonolithUIInternal::TryGetRequiredString(Params, TEXT("asset_path"), AssetPath, ParamError))
    {
        return ParamError;
    }

    FString WidgetClassName;
    if (!MonolithUIInternal::TryGetRequiredString(Params, TEXT("widget_class"), WidgetClassName, ParamError))
    {
        return ParamError;
    }

    FMonolithActionResult Err;
    UWidgetBlueprint* WBP = MonolithUIInternal::LoadWidgetBlueprint(AssetPath, Err);
    if (!WBP) return Err;
    if (!WBP->WidgetTree)
    {
        return FMonolithActionResult::Error(TEXT("WidgetTree is null"));
    }

    // Resolve widget class
    UClass* WidgetClass = MonolithUIInternal::WidgetClassFromName(WidgetClassName);
    if (!WidgetClass)
    {
        // Phase K — surface as FUISpecError. Common widget tokens go in
        // valid_options as a starter list; the full surface lives behind
        // ui::list_widget_types (referenced in suggested_fix).
        return MonolithUIInternal::MakeErrorFromSpecError(MonolithUIInternal::MakeSpecError(
            TEXT("Type"),
            TEXT("/widget_class"),
            FString::Printf(TEXT("Unknown widget class token: '%s'."), *WidgetClassName),
            TEXT("Call ui::list_widget_types for the full registered set, or pass a /Script/UMG.Class path."),
            { TEXT("CanvasPanel"), TEXT("VerticalBox"), TEXT("HorizontalBox"), TEXT("Overlay"),
              TEXT("TextBlock"), TEXT("RichTextBlock"), TEXT("Image"), TEXT("Button"),
              TEXT("Border"), TEXT("SizeBox"), TEXT("ProgressBar"), TEXT("CheckBox"),
              TEXT("Slider"), TEXT("EditableText"), TEXT("EditableTextBox"), TEXT("ScrollBox") }));
    }

    // Widget name
    FString WidgetName = MonolithUIInternal::GetOptionalString(Params, TEXT("widget_name"));
    FName WidgetFName = WidgetName.IsEmpty() ? NAME_None : FName(*WidgetName);

    // Find parent widget
    UPanelWidget* ParentPanel = nullptr;
    FString ParentName = MonolithUIInternal::GetOptionalString(Params, TEXT("parent_name"));
    if (ParentName.IsEmpty())
    {
        ParentPanel = Cast<UPanelWidget>(WBP->WidgetTree->RootWidget);
    }
    else
    {
        UWidget* Found = WBP->WidgetTree->FindWidget(FName(*ParentName));
        ParentPanel = Cast<UPanelWidget>(Found);
    }

    if (!ParentPanel)
    {
        if (ParentName.IsEmpty())
        {
            return MonolithUIInternal::MakeErrorFromSpecError(MonolithUIInternal::MakeSpecError(
                TEXT("Lookup"),
                TEXT("/parent_name"),
                TEXT("Widget Blueprint has no root panel widget. Create one first."),
                TEXT("Call ui::add_widget with a valid parent, or ensure the WBP has a CanvasPanel/VerticalBox root.")));
        }
        else
        {
            // Phase K — Lookup-class error. Cannot enumerate live widget names in
            // valid_options without scanning the WidgetTree (the suggested_fix
            // points the LLM at get_widget_tree for that lookup).
            return MonolithUIInternal::MakeErrorFromSpecError(MonolithUIInternal::MakeSpecError(
                TEXT("Lookup"),
                TEXT("/parent_name"),
                FString::Printf(TEXT("Parent '%s' not found or is not a panel widget."), *ParentName),
                TEXT("Call ui::get_widget_tree to enumerate live widget names; the parent must be a UPanelWidget subclass.")));
        }
    }

    WBP->Modify();
    WBP->WidgetTree->Modify();
    ParentPanel->Modify();

    // Construct widget
    UWidget* NewWidget = WBP->WidgetTree->ConstructWidget<UWidget>(WidgetClass, WidgetFName);
    if (!NewWidget)
    {
        return FMonolithActionResult::Error(
            FString::Printf(TEXT("Failed to construct widget of class %s"), *WidgetClassName));
    }
    NewWidget->Modify();

    // Add to parent.
    //
    // UPanelWidget::AddChild returns nullptr in three conditions
    // (Engine/Source/Runtime/UMG/Private/Components/PanelWidget.cpp:132-142):
    //   1. Content is null — impossible here; NewWidget was just constructed.
    //   2. !bCanHaveMultipleChildren && GetChildrenCount() > 0 — the single-child
    //      invariant on UContentWidget subclasses (Border, RoundedBorder,
    //      Button, SizeBox, ScaleBox, BackgroundBlur, InvalidationBox,
    //      RetainerBox, SafeZone, NamedSlot).
    //   3. (Subclass-specific rejections via OnSlotAdded, rare in practice.)
    //
    // When case 2 fires, callers routinely waste time staring at the opaque
    // message before realizing a VerticalBox/HorizontalBox wrapper is missing.
    // Classify it here so the error speaks for itself.
    UPanelSlot* Slot = ParentPanel->AddChild(NewWidget);
    if (!Slot)
    {
        if (!ParentPanel->CanHaveMultipleChildren() && ParentPanel->GetChildrenCount() > 0)
        {
            UWidget* ExistingChild = ParentPanel->GetChildAt(0);
            const FString ExistingName = ExistingChild ? ExistingChild->GetName() : TEXT("<unknown>");
            return FMonolithActionResult::Error(FString::Printf(
                TEXT("AddChild failed: parent '%s' is a single-child container (%s) and already holds '%s'. ")
                TEXT("Wrap additional children in a VerticalBox/HorizontalBox."),
                *ParentPanel->GetName(),
                *ParentPanel->GetClass()->GetName(),
                *ExistingName));
        }
        return FMonolithActionResult::Error(TEXT("AddChild returned null slot"));
    }
    Slot->Modify();

    // Configure canvas slot if applicable
    if (UCanvasPanelSlot* CSlot = Cast<UCanvasPanelSlot>(Slot))
    {
        // Anchor preset
        FString AnchorPreset = MonolithUIInternal::GetOptionalString(Params, TEXT("anchor_preset"));
        if (!AnchorPreset.IsEmpty())
        {
            CSlot->SetAnchors(MonolithUIInternal::GetAnchorPreset(AnchorPreset));
        }

        // Position
        const TSharedPtr<FJsonObject>* PosObj = nullptr;
        if (Params->TryGetObjectField(TEXT("position"), PosObj))
        {
            double Px = 0, Py = 0;
            (*PosObj)->TryGetNumberField(TEXT("x"), Px);
            (*PosObj)->TryGetNumberField(TEXT("y"), Py);
            FVector2D Pos(Px, Py);
            CSlot->SetPosition(Pos);
        }

        // Size
        const TSharedPtr<FJsonObject>* SizeObj = nullptr;
        if (Params->TryGetObjectField(TEXT("size"), SizeObj))
        {
            double Sx = 0, Sy = 0;
            (*SizeObj)->TryGetNumberField(TEXT("x"), Sx);
            (*SizeObj)->TryGetNumberField(TEXT("y"), Sy);
            FVector2D Size(Sx, Sy);
            CSlot->SetSize(Size);
        }

        // Auto-size
        CSlot->SetAutoSize(MonolithUIInternal::GetOptionalBool(Params, TEXT("auto_size"), CSlot->GetAutoSize()));
    }

    // Configure box/overlay slot alignment
    FString HAlign = MonolithUIInternal::GetOptionalString(Params, TEXT("h_align"));
    FString VAlign = MonolithUIInternal::GetOptionalString(Params, TEXT("v_align"));

    if (UVerticalBoxSlot* VS = Cast<UVerticalBoxSlot>(Slot))
    {
        if (!HAlign.IsEmpty())
        {
            EHorizontalAlignment HA = HAlign == TEXT("Left") ? HAlign_Left :
                                      HAlign == TEXT("Center") ? HAlign_Center :
                                      HAlign == TEXT("Right") ? HAlign_Right : HAlign_Fill;
            VS->SetHorizontalAlignment(HA);
        }
        if (!VAlign.IsEmpty())
        {
            EVerticalAlignment VA = VAlign == TEXT("Top") ? VAlign_Top :
                                    VAlign == TEXT("Center") ? VAlign_Center :
                                    VAlign == TEXT("Bottom") ? VAlign_Bottom : VAlign_Fill;
            VS->SetVerticalAlignment(VA);
        }
    }
    else if (UHorizontalBoxSlot* HS = Cast<UHorizontalBoxSlot>(Slot))
    {
        if (!HAlign.IsEmpty())
        {
            EHorizontalAlignment HA = HAlign == TEXT("Left") ? HAlign_Left :
                                      HAlign == TEXT("Center") ? HAlign_Center :
                                      HAlign == TEXT("Right") ? HAlign_Right : HAlign_Fill;
            HS->SetHorizontalAlignment(HA);
        }
        if (!VAlign.IsEmpty())
        {
            EVerticalAlignment VA = VAlign == TEXT("Top") ? VAlign_Top :
                                    VAlign == TEXT("Center") ? VAlign_Center :
                                    VAlign == TEXT("Bottom") ? VAlign_Bottom : VAlign_Fill;
            HS->SetVerticalAlignment(VA);
        }
    }
    else if (UOverlaySlot* OS = Cast<UOverlaySlot>(Slot))
    {
        if (!HAlign.IsEmpty())
        {
            EHorizontalAlignment HA = HAlign == TEXT("Left") ? HAlign_Left :
                                      HAlign == TEXT("Center") ? HAlign_Center :
                                      HAlign == TEXT("Right") ? HAlign_Right : HAlign_Fill;
            OS->SetHorizontalAlignment(HA);
        }
        if (!VAlign.IsEmpty())
        {
            EVerticalAlignment VA = VAlign == TEXT("Top") ? VAlign_Top :
                                    VAlign == TEXT("Center") ? VAlign_Center :
                                    VAlign == TEXT("Bottom") ? VAlign_Bottom : VAlign_Fill;
            OS->SetVerticalAlignment(VA);
        }
    }

    // Padding
    const TSharedPtr<FJsonObject>* PadObj = nullptr;
    if (Params->TryGetObjectField(TEXT("padding"), PadObj))
    {
        FMargin Pad;
        if (MonolithUIInternal::TryParseMargin(PadObj, Pad))
        {
            if (UVerticalBoxSlot* VS = Cast<UVerticalBoxSlot>(Slot)) VS->SetPadding(Pad);
            else if (UHorizontalBoxSlot* HS = Cast<UHorizontalBoxSlot>(Slot)) HS->SetPadding(Pad);
            else if (UOverlaySlot* OS = Cast<UOverlaySlot>(Slot)) OS->SetPadding(Pad);
        }
    }

    // Mirror editor bookkeeping so the compiler sees a GUID for the final widget name.
    MonolithUIInternal::RegisterCreatedWidget(WBP, NewWidget);

    // Mark modified
    WBP->WidgetTree->Modify();
    ParentPanel->Modify();
    NewWidget->Modify();
    FBlueprintEditorUtils::MarkBlueprintAsStructurallyModified(WBP);
    WBP->GetOutermost()->MarkPackageDirty();

    // Compile if requested
    const bool bCompile = MonolithUIInternal::GetOptionalBool(Params, TEXT("compile"), true);
    if (bCompile)
    {
        FKismetEditorUtilities::CompileBlueprint(WBP);
    }

    // Build result
    TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
    Result->SetStringField(TEXT("widget_name"), NewWidget->GetName());
    Result->SetStringField(TEXT("widget_class"), WidgetClassName);
    Result->SetStringField(TEXT("parent_name"), ParentPanel->GetName());
    Result->SetStringField(TEXT("slot_type"), Slot->GetClass()->GetName());
    Result->SetBoolField(TEXT("compiled"), bCompile);

    return FMonolithActionResult::Success(Result);
}

// --- add_extension_point_widget ---
FMonolithActionResult FMonolithUIActions::HandleAddExtensionPointWidget(const TSharedPtr<FJsonObject>& Params)
{
    FMonolithActionResult ParamError;
    FString AssetPath;
    if (!MonolithUIInternal::TryGetRequiredString(Params, TEXT("asset_path"), AssetPath, ParamError))
    {
        return ParamError;
    }

    FString WidgetName;
    if (!MonolithUIInternal::TryGetRequiredString(Params, TEXT("widget_name"), WidgetName, ParamError))
    {
        return ParamError;
    }

    FString ExtensionPointTagName;
    if (!MonolithUIInternal::TryGetRequiredString(Params, TEXT("extension_point_tag"), ExtensionPointTagName, ParamError))
    {
        return ParamError;
    }

    FGameplayTag ExtensionPointTag = FGameplayTag::RequestGameplayTag(FName(*ExtensionPointTagName), /*ErrorIfNotFound=*/false);
    if (!ExtensionPointTag.IsValid())
    {
        return FMonolithActionResult::Error(
            FString::Printf(TEXT("GameplayTag '%s' is not registered."), *ExtensionPointTagName),
            FMonolithJsonUtils::ErrInvalidParams);
    }

    FMonolithActionResult Err;
    UWidgetBlueprint* WBP = MonolithUIInternal::LoadWidgetBlueprint(AssetPath, Err);
    if (!WBP) return Err;
    if (!WBP->WidgetTree)
    {
        return FMonolithActionResult::Error(TEXT("WidgetTree is null"));
    }

    FString WidgetClassName = MonolithUIInternal::GetOptionalString(Params, TEXT("widget_class"), TEXT("/Script/UIExtension.UIExtensionPointWidget"));
    UClass* WidgetClass = MonolithUIInternal::WidgetClassFromName(WidgetClassName);
    if (!WidgetClass)
    {
        WidgetClass = StaticLoadClass(UWidget::StaticClass(), nullptr, *WidgetClassName);
    }
    if (!WidgetClass || !WidgetClass->IsChildOf(UWidget::StaticClass()))
    {
        return FMonolithActionResult::Error(
            FString::Printf(TEXT("Could not resolve a UWidget class for '%s'. Is the UIExtension plugin enabled?"), *WidgetClassName),
            FMonolithJsonUtils::ErrInvalidParams);
    }

    bool bChanged = false;
    bool bCreated = false;
    bool bRootCreated = false;

    if (!WBP->WidgetTree->RootWidget)
    {
        UClass* CanvasClass = MonolithUIInternal::WidgetClassFromName(TEXT("CanvasPanel"));
        if (!CanvasClass)
        {
            return FMonolithActionResult::Error(TEXT("Could not resolve CanvasPanel for empty WidgetTree root."), -32603);
        }
        WBP->Modify();
        WBP->WidgetTree->Modify();
        UWidget* Root = WBP->WidgetTree->ConstructWidget<UWidget>(CanvasClass, FName(TEXT("RootCanvas")));
        WBP->WidgetTree->RootWidget = Root;
        MonolithUIInternal::RegisterCreatedWidget(WBP, Root);
        bRootCreated = true;
        bChanged = true;
    }

    FString ParentName = MonolithUIInternal::GetOptionalString(Params, TEXT("parent_name"));
    UPanelWidget* ParentPanel = nullptr;
    if (ParentName.IsEmpty())
    {
        ParentPanel = Cast<UPanelWidget>(WBP->WidgetTree->RootWidget);
    }
    else
    {
        ParentPanel = Cast<UPanelWidget>(WBP->WidgetTree->FindWidget(FName(*ParentName)));
    }
    if (!ParentPanel)
    {
        return MonolithUIInternal::MakeErrorFromSpecError(MonolithUIInternal::MakeSpecError(
            TEXT("Lookup"),
            TEXT("/parent_name"),
            FString::Printf(TEXT("Parent '%s' not found or is not a panel widget."), *ParentName),
            TEXT("Call ui::get_widget_tree to enumerate live widget names; the parent must be a UPanelWidget subclass.")));
    }

    UWidget* ExtensionWidget = WBP->WidgetTree->FindWidget(FName(*WidgetName));
    if (ExtensionWidget && !ExtensionWidget->IsA(WidgetClass))
    {
        return FMonolithActionResult::Error(
            FString::Printf(TEXT("Widget '%s' already exists as '%s', not '%s'."),
                *WidgetName,
                *ExtensionWidget->GetClass()->GetPathName(),
                *WidgetClass->GetPathName()),
            FMonolithJsonUtils::ErrInvalidParams);
    }
    if (!ExtensionWidget)
    {
        WBP->Modify();
        WBP->WidgetTree->Modify();
        ParentPanel->Modify();
        ExtensionWidget = WBP->WidgetTree->ConstructWidget<UWidget>(WidgetClass, FName(*WidgetName));
        if (!ExtensionWidget)
        {
            return FMonolithActionResult::Error(
                FString::Printf(TEXT("Failed to construct widget of class '%s'."), *WidgetClass->GetPathName()),
                -32603);
        }
        UPanelSlot* AddedSlot = ParentPanel->AddChild(ExtensionWidget);
        if (!AddedSlot)
        {
            return FMonolithActionResult::Error(FString::Printf(
                TEXT("AddChild failed for parent '%s' (%s)."),
                *ParentPanel->GetName(),
                *ParentPanel->GetClass()->GetName()));
        }
        AddedSlot->Modify();
        MonolithUIInternal::RegisterCreatedWidget(WBP, ExtensionWidget);
        bCreated = true;
        bChanged = true;
    }

    if (ExtensionWidget->GetParent() != ParentPanel)
    {
        UPanelWidget* OldParent = ExtensionWidget->GetParent();
        if (OldParent)
        {
            OldParent->Modify();
            OldParent->RemoveChild(ExtensionWidget);
        }
        ParentPanel->Modify();
        UPanelSlot* AddedSlot = ParentPanel->AddChild(ExtensionWidget);
        if (!AddedSlot)
        {
            return FMonolithActionResult::Error(FString::Printf(
                TEXT("AddChild failed while moving '%s' to parent '%s'."),
                *WidgetName,
                *ParentPanel->GetName()));
        }
        AddedSlot->Modify();
        bChanged = true;
    }

    FStructProperty* TagProperty = FindFProperty<FStructProperty>(ExtensionWidget->GetClass(), TEXT("ExtensionPointTag"));
    if (!TagProperty || TagProperty->Struct != FGameplayTag::StaticStruct())
    {
        return FMonolithActionResult::Error(
            FString::Printf(TEXT("Widget class '%s' does not expose FGameplayTag property ExtensionPointTag."), *ExtensionWidget->GetClass()->GetPathName()),
            FMonolithJsonUtils::ErrInvalidParams);
    }
    FGameplayTag* TagValue = TagProperty->ContainerPtrToValuePtr<FGameplayTag>(ExtensionWidget);
    if (!TagValue || *TagValue != ExtensionPointTag)
    {
        ExtensionWidget->Modify();
        *TagValue = ExtensionPointTag;
        bChanged = true;
    }

    UPanelSlot* Slot = ExtensionWidget->Slot;
    bool bSlotChanged = false;
    auto ParseVec2 = [Params](const TCHAR* FieldName, FVector2D& OutValue) -> bool
    {
        const TSharedPtr<FJsonObject>* Obj = nullptr;
        if (!Params.IsValid() || !Params->TryGetObjectField(FieldName, Obj) || !Obj)
        {
            return false;
        }
        double X = OutValue.X;
        double Y = OutValue.Y;
        (*Obj)->TryGetNumberField(TEXT("x"), X);
        (*Obj)->TryGetNumberField(TEXT("y"), Y);
        OutValue = FVector2D(X, Y);
        return true;
    };
    auto ParseHAlign = [](const FString& Value)
    {
        if (Value.Equals(TEXT("Left"), ESearchCase::IgnoreCase)) return HAlign_Left;
        if (Value.Equals(TEXT("Center"), ESearchCase::IgnoreCase)) return HAlign_Center;
        if (Value.Equals(TEXT("Right"), ESearchCase::IgnoreCase)) return HAlign_Right;
        return HAlign_Fill;
    };
    auto ParseVAlign = [](const FString& Value)
    {
        if (Value.Equals(TEXT("Top"), ESearchCase::IgnoreCase)) return VAlign_Top;
        if (Value.Equals(TEXT("Center"), ESearchCase::IgnoreCase)) return VAlign_Center;
        if (Value.Equals(TEXT("Bottom"), ESearchCase::IgnoreCase)) return VAlign_Bottom;
        return VAlign_Fill;
    };

    if (UCanvasPanelSlot* CSlot = Cast<UCanvasPanelSlot>(Slot))
    {
        FString AnchorPreset = MonolithUIInternal::GetOptionalString(Params, TEXT("anchor_preset"));
        if (!AnchorPreset.IsEmpty())
        {
            const FAnchors DesiredAnchors = MonolithUIInternal::GetAnchorPreset(AnchorPreset);
            if (CSlot->GetAnchors().Minimum != DesiredAnchors.Minimum || CSlot->GetAnchors().Maximum != DesiredAnchors.Maximum)
            {
                CSlot->Modify();
                CSlot->SetAnchors(DesiredAnchors);
                bSlotChanged = true;
            }
        }

        FVector2D Position = CSlot->GetPosition();
        if (ParseVec2(TEXT("position"), Position) && CSlot->GetPosition() != Position)
        {
            CSlot->Modify();
            CSlot->SetPosition(Position);
            bSlotChanged = true;
        }

        FVector2D Size = CSlot->GetSize();
        if (ParseVec2(TEXT("size"), Size) && CSlot->GetSize() != Size)
        {
            CSlot->Modify();
            CSlot->SetSize(Size);
            bSlotChanged = true;
        }

        FVector2D Alignment = CSlot->GetAlignment();
        if (ParseVec2(TEXT("alignment"), Alignment) && CSlot->GetAlignment() != Alignment)
        {
            CSlot->Modify();
            CSlot->SetAlignment(Alignment);
            bSlotChanged = true;
        }

        const bool bAutoSize = MonolithUIInternal::GetOptionalBool(Params, TEXT("auto_size"), CSlot->GetAutoSize());
        if (CSlot->GetAutoSize() != bAutoSize)
        {
            CSlot->Modify();
            CSlot->SetAutoSize(bAutoSize);
            bSlotChanged = true;
        }
    }

    FString HAlign = MonolithUIInternal::GetOptionalString(Params, TEXT("h_align"));
    FString VAlign = MonolithUIInternal::GetOptionalString(Params, TEXT("v_align"));
    const TSharedPtr<FJsonObject>* PadObj = nullptr;
    const bool bHasPadding = Params.IsValid() && Params->TryGetObjectField(TEXT("padding"), PadObj);
    FMargin Pad;
    const bool bParsedPadding = bHasPadding && MonolithUIInternal::TryParseMargin(PadObj, Pad);

    if (UVerticalBoxSlot* VS = Cast<UVerticalBoxSlot>(Slot))
    {
        if (!HAlign.IsEmpty() && VS->GetHorizontalAlignment() != ParseHAlign(HAlign)) { VS->Modify(); VS->SetHorizontalAlignment(ParseHAlign(HAlign)); bSlotChanged = true; }
        if (!VAlign.IsEmpty() && VS->GetVerticalAlignment() != ParseVAlign(VAlign)) { VS->Modify(); VS->SetVerticalAlignment(ParseVAlign(VAlign)); bSlotChanged = true; }
        if (bParsedPadding && VS->GetPadding() != Pad) { VS->Modify(); VS->SetPadding(Pad); bSlotChanged = true; }
    }
    else if (UHorizontalBoxSlot* HS = Cast<UHorizontalBoxSlot>(Slot))
    {
        if (!HAlign.IsEmpty() && HS->GetHorizontalAlignment() != ParseHAlign(HAlign)) { HS->Modify(); HS->SetHorizontalAlignment(ParseHAlign(HAlign)); bSlotChanged = true; }
        if (!VAlign.IsEmpty() && HS->GetVerticalAlignment() != ParseVAlign(VAlign)) { HS->Modify(); HS->SetVerticalAlignment(ParseVAlign(VAlign)); bSlotChanged = true; }
        if (bParsedPadding && HS->GetPadding() != Pad) { HS->Modify(); HS->SetPadding(Pad); bSlotChanged = true; }
    }
    else if (UOverlaySlot* OS = Cast<UOverlaySlot>(Slot))
    {
        if (!HAlign.IsEmpty() && OS->GetHorizontalAlignment() != ParseHAlign(HAlign)) { OS->Modify(); OS->SetHorizontalAlignment(ParseHAlign(HAlign)); bSlotChanged = true; }
        if (!VAlign.IsEmpty() && OS->GetVerticalAlignment() != ParseVAlign(VAlign)) { OS->Modify(); OS->SetVerticalAlignment(ParseVAlign(VAlign)); bSlotChanged = true; }
        if (bParsedPadding && OS->GetPadding() != Pad) { OS->Modify(); OS->SetPadding(Pad); bSlotChanged = true; }
    }

    if (bSlotChanged)
    {
        bChanged = true;
    }

    const bool bCompile = MonolithUIInternal::GetOptionalBool(Params, TEXT("compile"), true);
    bool bCompiled = false;
    bool bSaved = false;
    if (bChanged)
    {
        WBP->Modify();
        WBP->WidgetTree->Modify();
        ParentPanel->Modify();
        ExtensionWidget->Modify();
        FBlueprintEditorUtils::MarkBlueprintAsStructurallyModified(WBP);
        WBP->GetOutermost()->MarkPackageDirty();

        if (bCompile)
        {
            FKismetEditorUtilities::CompileBlueprint(WBP);
            bCompiled = true;
        }

        const bool bSave = MonolithUIInternal::GetOptionalBool(Params, TEXT("save"), false);
        if (bSave)
        {
            FSavePackageArgs SaveArgs;
            SaveArgs.TopLevelFlags = RF_Public | RF_Standalone;
            bSaved = UPackage::SavePackage(
                WBP->GetOutermost(),
                WBP,
                *FPackageName::LongPackageNameToFilename(WBP->GetOutermost()->GetName(), FPackageName::GetAssetPackageExtension()),
                SaveArgs);
            if (!bSaved)
            {
                return FMonolithActionResult::Error(
                    FString::Printf(TEXT("SavePackage failed for '%s'."), *WBP->GetOutermost()->GetName()),
                    -32603);
            }
        }
    }

    TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
    Result->SetBoolField(TEXT("changed"), bChanged);
    Result->SetBoolField(TEXT("created"), bCreated);
    Result->SetBoolField(TEXT("root_created"), bRootCreated);
    Result->SetBoolField(TEXT("slot_changed"), bSlotChanged);
    Result->SetBoolField(TEXT("compiled"), bCompiled);
    Result->SetBoolField(TEXT("saved"), bSaved);
    Result->SetStringField(TEXT("asset_path"), WBP->GetPathName());
    Result->SetStringField(TEXT("widget_name"), ExtensionWidget->GetName());
    Result->SetStringField(TEXT("widget_class"), ExtensionWidget->GetClass()->GetPathName());
    Result->SetStringField(TEXT("parent_name"), ParentPanel->GetName());
    Result->SetStringField(TEXT("extension_point_tag"), ExtensionPointTag.ToString());
    Result->SetStringField(TEXT("slot_type"), Slot ? Slot->GetClass()->GetName() : FString());
    return FMonolithActionResult::Success(Result);
}

// --- add_primary_game_layout_layer ---
FMonolithActionResult FMonolithUIActions::HandleAddPrimaryGameLayoutLayer(const TSharedPtr<FJsonObject>& Params)
{
    using namespace MonolithUICommonFrameworkInternal;

    FMonolithActionResult ParamError;
    FString AssetPath;
    if (!MonolithUIInternal::TryGetRequiredString(Params, TEXT("asset_path"), AssetPath, ParamError))
    {
        return ParamError;
    }

    FString LayerTagName;
    if (!MonolithUIInternal::TryGetRequiredString(Params, TEXT("layer_tag"), LayerTagName, ParamError))
    {
        return ParamError;
    }

    FGameplayTag LayerTag = FGameplayTag::RequestGameplayTag(FName(*LayerTagName), /*ErrorIfNotFound=*/false);
    if (!LayerTag.IsValid())
    {
        return FMonolithActionResult::Error(
            FString::Printf(TEXT("GameplayTag '%s' is not registered."), *LayerTagName),
            FMonolithJsonUtils::ErrInvalidParams);
    }

    FMonolithActionResult Err;
    UWidgetBlueprint* WBP = MonolithUIInternal::LoadWidgetBlueprint(AssetPath, Err);
    if (!WBP)
    {
        return Err;
    }
    if (!WBP->WidgetTree)
    {
        return FMonolithActionResult::Error(TEXT("WidgetTree is null"));
    }

    UClass* PrimaryGameLayoutClass = LoadClassPath(TEXT("/Script/CommonGame.PrimaryGameLayout"));
    if (!PrimaryGameLayoutClass)
    {
        return FMonolithActionResult::Error(
            TEXT("CommonGame.PrimaryGameLayout is unavailable. Enable the CommonGame plugin before adding PrimaryGameLayout layers."),
            FMonolithJsonUtils::ErrInvalidParams);
    }
    if (!WBP->ParentClass || !WBP->ParentClass->IsChildOf(PrimaryGameLayoutClass))
    {
        return FMonolithActionResult::Error(
            FString::Printf(
                TEXT("Widget Blueprint '%s' parent '%s' is not a child of CommonGame.PrimaryGameLayout."),
                *AssetPath,
                WBP->ParentClass ? *WBP->ParentClass->GetPathName() : TEXT("<null>")),
            FMonolithJsonUtils::ErrInvalidParams);
    }

    UClass* ContainerBaseClass = LoadClassPath(TEXT("/Script/CommonUI.CommonActivatableWidgetContainerBase"));
    if (!ContainerBaseClass)
    {
        return FMonolithActionResult::Error(
            TEXT("CommonUI.CommonActivatableWidgetContainerBase is unavailable. Enable the CommonUI plugin before adding activatable layer containers."),
            FMonolithJsonUtils::ErrInvalidParams);
    }

    FString WidgetClassName = MonolithUIInternal::GetOptionalString(Params, TEXT("widget_class"), TEXT("/Script/CommonUI.CommonActivatableWidgetStack"));
    UClass* WidgetClass = MonolithUIInternal::WidgetClassFromName(WidgetClassName);
    if (!WidgetClass)
    {
        WidgetClass = StaticLoadClass(UWidget::StaticClass(), nullptr, *WidgetClassName);
    }
    if (!WidgetClass || !WidgetClass->IsChildOf(UWidget::StaticClass()))
    {
        return FMonolithActionResult::Error(
            FString::Printf(TEXT("Could not resolve a UWidget class for '%s'."), *WidgetClassName),
            FMonolithJsonUtils::ErrInvalidParams);
    }
    if (!WidgetClass->IsChildOf(ContainerBaseClass))
    {
        return FMonolithActionResult::Error(
            FString::Printf(
                TEXT("Widget class '%s' is not a child of CommonActivatableWidgetContainerBase."),
                *WidgetClass->GetPathName()),
            FMonolithJsonUtils::ErrInvalidParams);
    }

    FString WidgetName = MonolithUIInternal::GetOptionalString(Params, TEXT("widget_name"));
    if (WidgetName.IsEmpty())
    {
        WidgetName = LayerTagName;
        int32 LastDot = INDEX_NONE;
        if (WidgetName.FindLastChar(TEXT('.'), LastDot))
        {
            WidgetName = WidgetName.RightChop(LastDot + 1);
        }
        WidgetName += TEXT("Stack");
    }

    bool bChanged = false;
    bool bCreated = false;
    bool bRootCreated = false;
    if (!WBP->WidgetTree->RootWidget)
    {
        UClass* CanvasClass = MonolithUIInternal::WidgetClassFromName(TEXT("CanvasPanel"));
        if (!CanvasClass)
        {
            return FMonolithActionResult::Error(TEXT("Could not resolve CanvasPanel for empty WidgetTree root."), -32603);
        }

        WBP->Modify();
        WBP->WidgetTree->Modify();
        UWidget* Root = WBP->WidgetTree->ConstructWidget<UWidget>(CanvasClass, FName(TEXT("RootCanvas")));
        WBP->WidgetTree->RootWidget = Root;
        MonolithUIInternal::RegisterCreatedWidget(WBP, Root);
        bRootCreated = true;
        bChanged = true;
    }

    FString ParentName = MonolithUIInternal::GetOptionalString(Params, TEXT("parent_name"));
    UPanelWidget* ParentPanel = nullptr;
    if (ParentName.IsEmpty())
    {
        ParentPanel = Cast<UPanelWidget>(WBP->WidgetTree->RootWidget);
    }
    else
    {
        ParentPanel = Cast<UPanelWidget>(WBP->WidgetTree->FindWidget(FName(*ParentName)));
    }
    if (!ParentPanel)
    {
        return MonolithUIInternal::MakeErrorFromSpecError(MonolithUIInternal::MakeSpecError(
            TEXT("Lookup"),
            TEXT("/parent_name"),
            FString::Printf(TEXT("Parent '%s' not found or is not a panel widget."), *ParentName),
            TEXT("Call ui::get_widget_tree to enumerate live widget names; the parent must be a UPanelWidget subclass.")));
    }

    UWidget* LayerWidget = WBP->WidgetTree->FindWidget(FName(*WidgetName));
    if (LayerWidget && !LayerWidget->IsA(WidgetClass))
    {
        return FMonolithActionResult::Error(
            FString::Printf(
                TEXT("Widget '%s' already exists as '%s', not '%s'."),
                *WidgetName,
                *LayerWidget->GetClass()->GetPathName(),
                *WidgetClass->GetPathName()),
            FMonolithJsonUtils::ErrInvalidParams);
    }

    if (!LayerWidget)
    {
        WBP->Modify();
        WBP->WidgetTree->Modify();
        ParentPanel->Modify();
        LayerWidget = WBP->WidgetTree->ConstructWidget<UWidget>(WidgetClass, FName(*WidgetName));
        if (!LayerWidget)
        {
            return FMonolithActionResult::Error(
                FString::Printf(TEXT("Failed to construct widget of class '%s'."), *WidgetClass->GetPathName()),
                -32603);
        }

        UPanelSlot* AddedSlot = ParentPanel->AddChild(LayerWidget);
        if (!AddedSlot)
        {
            return FMonolithActionResult::Error(FString::Printf(
                TEXT("AddChild failed for parent '%s' (%s)."),
                *ParentPanel->GetName(),
                *ParentPanel->GetClass()->GetName()));
        }
        AddedSlot->Modify();
        MonolithUIInternal::RegisterCreatedWidget(WBP, LayerWidget);
        bCreated = true;
        bChanged = true;
    }

    if (LayerWidget->GetParent() != ParentPanel)
    {
        UPanelWidget* OldParent = LayerWidget->GetParent();
        if (OldParent)
        {
            OldParent->Modify();
            OldParent->RemoveChild(LayerWidget);
        }
        ParentPanel->Modify();
        UPanelSlot* AddedSlot = ParentPanel->AddChild(LayerWidget);
        if (!AddedSlot)
        {
            return FMonolithActionResult::Error(FString::Printf(
                TEXT("AddChild failed while moving '%s' to parent '%s'."),
                *WidgetName,
                *ParentPanel->GetName()));
        }
        AddedSlot->Modify();
        bChanged = true;
    }

    UPanelSlot* Slot = LayerWidget->Slot;
    bool bSlotChanged = false;
    auto ParseVec2 = [Params](const TCHAR* FieldName, FVector2D& OutValue) -> bool
    {
        const TSharedPtr<FJsonObject>* Obj = nullptr;
        if (!Params.IsValid() || !Params->TryGetObjectField(FieldName, Obj) || !Obj)
        {
            return false;
        }
        double X = OutValue.X;
        double Y = OutValue.Y;
        (*Obj)->TryGetNumberField(TEXT("x"), X);
        (*Obj)->TryGetNumberField(TEXT("y"), Y);
        OutValue = FVector2D(X, Y);
        return true;
    };
    auto ParseHAlign = [](const FString& Value)
    {
        if (Value.Equals(TEXT("Left"), ESearchCase::IgnoreCase)) return HAlign_Left;
        if (Value.Equals(TEXT("Center"), ESearchCase::IgnoreCase)) return HAlign_Center;
        if (Value.Equals(TEXT("Right"), ESearchCase::IgnoreCase)) return HAlign_Right;
        return HAlign_Fill;
    };
    auto ParseVAlign = [](const FString& Value)
    {
        if (Value.Equals(TEXT("Top"), ESearchCase::IgnoreCase)) return VAlign_Top;
        if (Value.Equals(TEXT("Center"), ESearchCase::IgnoreCase)) return VAlign_Center;
        if (Value.Equals(TEXT("Bottom"), ESearchCase::IgnoreCase)) return VAlign_Bottom;
        return VAlign_Fill;
    };

    if (UCanvasPanelSlot* CSlot = Cast<UCanvasPanelSlot>(Slot))
    {
        FString AnchorPreset = MonolithUIInternal::GetOptionalString(Params, TEXT("anchor_preset"));
        if (!AnchorPreset.IsEmpty())
        {
            const FAnchors DesiredAnchors = MonolithUIInternal::GetAnchorPreset(AnchorPreset);
            if (CSlot->GetAnchors().Minimum != DesiredAnchors.Minimum || CSlot->GetAnchors().Maximum != DesiredAnchors.Maximum)
            {
                CSlot->Modify();
                CSlot->SetAnchors(DesiredAnchors);
                bSlotChanged = true;
            }
        }

        FVector2D Position = CSlot->GetPosition();
        if (ParseVec2(TEXT("position"), Position) && CSlot->GetPosition() != Position)
        {
            CSlot->Modify();
            CSlot->SetPosition(Position);
            bSlotChanged = true;
        }

        FVector2D Size = CSlot->GetSize();
        if (ParseVec2(TEXT("size"), Size) && CSlot->GetSize() != Size)
        {
            CSlot->Modify();
            CSlot->SetSize(Size);
            bSlotChanged = true;
        }

        FVector2D Alignment = CSlot->GetAlignment();
        if (ParseVec2(TEXT("alignment"), Alignment) && CSlot->GetAlignment() != Alignment)
        {
            CSlot->Modify();
            CSlot->SetAlignment(Alignment);
            bSlotChanged = true;
        }

        const bool bAutoSize = MonolithUIInternal::GetOptionalBool(Params, TEXT("auto_size"), CSlot->GetAutoSize());
        if (CSlot->GetAutoSize() != bAutoSize)
        {
            CSlot->Modify();
            CSlot->SetAutoSize(bAutoSize);
            bSlotChanged = true;
        }
    }

    FString HAlign = MonolithUIInternal::GetOptionalString(Params, TEXT("h_align"));
    FString VAlign = MonolithUIInternal::GetOptionalString(Params, TEXT("v_align"));
    const TSharedPtr<FJsonObject>* PadObj = nullptr;
    const bool bHasPadding = Params.IsValid() && Params->TryGetObjectField(TEXT("padding"), PadObj);
    FMargin Pad;
    const bool bParsedPadding = bHasPadding && MonolithUIInternal::TryParseMargin(PadObj, Pad);

    if (UVerticalBoxSlot* VS = Cast<UVerticalBoxSlot>(Slot))
    {
        if (!HAlign.IsEmpty() && VS->GetHorizontalAlignment() != ParseHAlign(HAlign)) { VS->Modify(); VS->SetHorizontalAlignment(ParseHAlign(HAlign)); bSlotChanged = true; }
        if (!VAlign.IsEmpty() && VS->GetVerticalAlignment() != ParseVAlign(VAlign)) { VS->Modify(); VS->SetVerticalAlignment(ParseVAlign(VAlign)); bSlotChanged = true; }
        if (bParsedPadding && VS->GetPadding() != Pad) { VS->Modify(); VS->SetPadding(Pad); bSlotChanged = true; }
    }
    else if (UHorizontalBoxSlot* HS = Cast<UHorizontalBoxSlot>(Slot))
    {
        if (!HAlign.IsEmpty() && HS->GetHorizontalAlignment() != ParseHAlign(HAlign)) { HS->Modify(); HS->SetHorizontalAlignment(ParseHAlign(HAlign)); bSlotChanged = true; }
        if (!VAlign.IsEmpty() && HS->GetVerticalAlignment() != ParseVAlign(VAlign)) { HS->Modify(); HS->SetVerticalAlignment(ParseVAlign(VAlign)); bSlotChanged = true; }
        if (bParsedPadding && HS->GetPadding() != Pad) { HS->Modify(); HS->SetPadding(Pad); bSlotChanged = true; }
    }
    else if (UOverlaySlot* OS = Cast<UOverlaySlot>(Slot))
    {
        if (!HAlign.IsEmpty() && OS->GetHorizontalAlignment() != ParseHAlign(HAlign)) { OS->Modify(); OS->SetHorizontalAlignment(ParseHAlign(HAlign)); bSlotChanged = true; }
        if (!VAlign.IsEmpty() && OS->GetVerticalAlignment() != ParseVAlign(VAlign)) { OS->Modify(); OS->SetVerticalAlignment(ParseVAlign(VAlign)); bSlotChanged = true; }
        if (bParsedPadding && OS->GetPadding() != Pad) { OS->Modify(); OS->SetPadding(Pad); bSlotChanged = true; }
    }

    if (bSlotChanged)
    {
        bChanged = true;
    }

    const bool bCompile = MonolithUIInternal::GetOptionalBool(Params, TEXT("compile"), true);
    bool bCompiled = false;
    bool bSaved = false;
    if (bChanged)
    {
        WBP->Modify();
        WBP->WidgetTree->Modify();
        ParentPanel->Modify();
        LayerWidget->Modify();
        FBlueprintEditorUtils::MarkBlueprintAsStructurallyModified(WBP);
        WBP->GetOutermost()->MarkPackageDirty();

        if (bCompile)
        {
            FKismetEditorUtilities::CompileBlueprint(WBP);
            bCompiled = true;
        }

        const bool bSave = MonolithUIInternal::GetOptionalBool(Params, TEXT("save"), false);
        if (bSave)
        {
            FSavePackageArgs SaveArgs;
            SaveArgs.TopLevelFlags = RF_Public | RF_Standalone;
            bSaved = UPackage::SavePackage(
                WBP->GetOutermost(),
                WBP,
                *FPackageName::LongPackageNameToFilename(WBP->GetOutermost()->GetName(), FPackageName::GetAssetPackageExtension()),
                SaveArgs);
            if (!bSaved)
            {
                return FMonolithActionResult::Error(
                    FString::Printf(TEXT("SavePackage failed for '%s'."), *WBP->GetOutermost()->GetName()),
                    -32603);
            }
        }
    }

    TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
    Result->SetBoolField(TEXT("changed"), bChanged);
    Result->SetBoolField(TEXT("created"), bCreated);
    Result->SetBoolField(TEXT("root_created"), bRootCreated);
    Result->SetBoolField(TEXT("slot_changed"), bSlotChanged);
    Result->SetBoolField(TEXT("compiled"), bCompiled);
    Result->SetBoolField(TEXT("saved"), bSaved);
    Result->SetStringField(TEXT("asset_path"), WBP->GetPathName());
    Result->SetStringField(TEXT("widget_name"), LayerWidget->GetName());
    Result->SetStringField(TEXT("widget_class"), LayerWidget->GetClass()->GetPathName());
    Result->SetStringField(TEXT("parent_name"), ParentPanel->GetName());
    Result->SetStringField(TEXT("layer_tag"), LayerTag.ToString());
    Result->SetStringField(TEXT("slot_type"), Slot ? Slot->GetClass()->GetName() : FString());
    Result->SetBoolField(TEXT("register_layer_call_required"), true);
    Result->SetStringField(TEXT("register_layer_function"), TEXT("RegisterLayer"));
    Result->SetStringField(TEXT("register_layer_tag"), LayerTag.ToString());
    Result->SetStringField(TEXT("register_layer_widget_name"), LayerWidget->GetName());
    Result->SetBoolField(TEXT("register_layer_function_found"), PrimaryGameLayoutClass->FindFunctionByName(FName(TEXT("RegisterLayer"))) != nullptr);
    return FMonolithActionResult::Success(Result);
}

FMonolithActionResult FMonolithUIActions::HandleGetCommonFrameworkStatus(const TSharedPtr<FJsonObject>& Params)
{
    using namespace MonolithUICommonFrameworkInternal;

    const bool bIncludeProperties = MonolithUIInternal::GetOptionalBool(Params, TEXT("include_properties"), false);
    const bool bIncludeFunctions = MonolithUIInternal::GetOptionalBool(Params, TEXT("include_functions"), false);
    const int32 PropertyLimit = GetOptionalInt(Params, TEXT("property_limit"), 40, 1, 200);
    const int32 FunctionLimit = GetOptionalInt(Params, TEXT("function_limit"), 80, 1, 300);

    TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
    Result->SetBoolField(TEXT("common_ui_available"), LoadClassPath(TEXT("/Script/CommonUI.CommonUserWidget")) != nullptr);
    Result->SetBoolField(TEXT("common_game_available"), LoadClassPath(TEXT("/Script/CommonGame.PrimaryGameLayout")) != nullptr);
    Result->SetBoolField(TEXT("ui_extension_available"), LoadClassPath(TEXT("/Script/UIExtension.UIExtensionPointWidget")) != nullptr);
    Result->SetBoolField(TEXT("common_user_available"), LoadClassPath(TEXT("/Script/CommonUser.CommonUserSubsystem")) != nullptr);
    Result->SetBoolField(TEXT("common_loading_screen_available"), LoadClassPath(TEXT("/Script/CommonLoadingScreen.LoadingScreenManager")) != nullptr);
    Result->SetBoolField(TEXT("game_settings_available"), LoadClassPath(TEXT("/Script/GameSettings.GameSettingRegistry")) != nullptr);
    Result->SetBoolField(TEXT("gameplay_message_router_available"), LoadClassPath(TEXT("/Script/GameplayMessageRuntime.GameplayMessageSubsystem")) != nullptr);
    Result->SetBoolField(TEXT("modular_gameplay_actors_available"), LoadClassPath(TEXT("/Script/ModularGameplayActors.ModularCharacter")) != nullptr);
    Result->SetBoolField(TEXT("game_subtitles_available"), LoadClassPath(TEXT("/Script/GameSubtitles.SubtitleDisplaySubsystem")) != nullptr);
    Result->SetBoolField(TEXT("uses_hard_dependencies"), false);

    TArray<TSharedPtr<FJsonValue>> Plugins;
    Plugins.Add(MakeShared<FJsonValueObject>(PluginStatus(TEXT("CommonUI"))));
    Plugins.Add(MakeShared<FJsonValueObject>(PluginStatus(TEXT("CommonGame"))));
    Plugins.Add(MakeShared<FJsonValueObject>(PluginStatus(TEXT("UIExtension"))));
    Plugins.Add(MakeShared<FJsonValueObject>(PluginStatus(TEXT("CommonUser"))));
    Plugins.Add(MakeShared<FJsonValueObject>(PluginStatus(TEXT("CommonLoadingScreen"))));
    Plugins.Add(MakeShared<FJsonValueObject>(PluginStatus(TEXT("GameSettings"))));
    Plugins.Add(MakeShared<FJsonValueObject>(PluginStatus(TEXT("GameplayMessageRouter"))));
    Plugins.Add(MakeShared<FJsonValueObject>(PluginStatus(TEXT("ModularGameplayActors"))));
    Plugins.Add(MakeShared<FJsonValueObject>(PluginStatus(TEXT("GameSubtitles"))));
    Result->SetArrayField(TEXT("plugins"), Plugins);

    TArray<TSharedPtr<FJsonValue>> Modules;
    Modules.Add(MakeShared<FJsonValueObject>(ModuleStatus(TEXT("CommonUI"))));
    Modules.Add(MakeShared<FJsonValueObject>(ModuleStatus(TEXT("CommonGame"))));
    Modules.Add(MakeShared<FJsonValueObject>(ModuleStatus(TEXT("UIExtension"))));
    Modules.Add(MakeShared<FJsonValueObject>(ModuleStatus(TEXT("CommonUser"))));
    Modules.Add(MakeShared<FJsonValueObject>(ModuleStatus(TEXT("CommonLoadingScreen"))));
    Modules.Add(MakeShared<FJsonValueObject>(ModuleStatus(TEXT("CommonStartupLoadingScreen"))));
    Modules.Add(MakeShared<FJsonValueObject>(ModuleStatus(TEXT("GameSettings"))));
    Modules.Add(MakeShared<FJsonValueObject>(ModuleStatus(TEXT("GameplayMessageRuntime"))));
    Modules.Add(MakeShared<FJsonValueObject>(ModuleStatus(TEXT("GameplayMessageNodes"))));
    Modules.Add(MakeShared<FJsonValueObject>(ModuleStatus(TEXT("ModularGameplayActors"))));
    Modules.Add(MakeShared<FJsonValueObject>(ModuleStatus(TEXT("GameSubtitles"))));
    Result->SetArrayField(TEXT("modules"), Modules);

    TArray<TSharedPtr<FJsonValue>> Classes;
    for (const FCommonClassSpec& Spec : CommonClassSpecs)
    {
        Classes.Add(MakeShared<FJsonValueObject>(ClassSummary(Spec, bIncludeProperties, bIncludeFunctions, PropertyLimit, FunctionLimit)));
    }
    Result->SetArrayField(TEXT("classes"), Classes);

    TArray<TSharedPtr<FJsonValue>> Structs;
    for (const FCommonStructSpec& Spec : CommonStructSpecs)
    {
        Structs.Add(MakeShared<FJsonValueObject>(StructSummary(Spec, bIncludeProperties, PropertyLimit)));
    }
    Result->SetArrayField(TEXT("structs"), Structs);

    return FMonolithActionResult::Success(Result);
}

FMonolithActionResult FMonolithUIActions::HandleDescribeCommonWidgetBlueprint(const TSharedPtr<FJsonObject>& Params)
{
    using namespace MonolithUICommonFrameworkInternal;

    FMonolithActionResult ParamError;
    FString AssetPath;
    if (!MonolithUIInternal::TryGetRequiredString(Params, TEXT("asset_path"), AssetPath, ParamError))
    {
        return ParamError;
    }

    FMonolithActionResult Err;
    UWidgetBlueprint* WBP = MonolithUIInternal::LoadWidgetBlueprint(AssetPath, Err);
    if (!WBP)
    {
        return Err;
    }
    if (!WBP->WidgetTree)
    {
        return FMonolithActionResult::Error(TEXT("WidgetTree is null (editor-only data not available)"));
    }

    const bool bIncludeExtensionPoints = MonolithUIInternal::GetOptionalBool(Params, TEXT("include_extension_points"), true);
    const bool bIncludeLayerCandidates = MonolithUIInternal::GetOptionalBool(Params, TEXT("include_layer_candidates"), true);
    const bool bIncludeWidgetTree = MonolithUIInternal::GetOptionalBool(Params, TEXT("include_widget_tree"), false);

    UClass* PrimaryGameLayoutClass = LoadClassPath(TEXT("/Script/CommonGame.PrimaryGameLayout"));
    UClass* ExtensionPointWidgetClass = LoadClassPath(TEXT("/Script/UIExtension.UIExtensionPointWidget"));
    UClass* ActivatableContainerClass = LoadClassPath(TEXT("/Script/CommonUI.CommonActivatableWidgetContainerBase"));

    TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
    Result->SetStringField(TEXT("asset_path"), AssetPath);
    Result->SetStringField(TEXT("parent_class"), WBP->ParentClass ? WBP->ParentClass->GetName() : FString());
    Result->SetStringField(TEXT("parent_class_path"), WBP->ParentClass ? WBP->ParentClass->GetPathName() : FString());
    Result->SetBoolField(TEXT("common_game_available"), PrimaryGameLayoutClass != nullptr);
    Result->SetBoolField(TEXT("ui_extension_available"), ExtensionPointWidgetClass != nullptr);
    Result->SetBoolField(TEXT("common_ui_available"), ActivatableContainerClass != nullptr);
    Result->SetBoolField(TEXT("is_primary_game_layout"), WBP->ParentClass && PrimaryGameLayoutClass && WBP->ParentClass->IsChildOf(PrimaryGameLayoutClass));

    TArray<UWidget*> Widgets;
    WBP->WidgetTree->GetAllWidgets(Widgets);
    Result->SetNumberField(TEXT("widget_count"), Widgets.Num());

    TArray<TSharedPtr<FJsonValue>> Warnings;
    if (!PrimaryGameLayoutClass)
    {
        Warnings.Add(MakeShared<FJsonValueString>(TEXT("CommonGame.PrimaryGameLayout class is unavailable; parentage check is limited.")));
    }
    if (bIncludeExtensionPoints && !ExtensionPointWidgetClass)
    {
        Warnings.Add(MakeShared<FJsonValueString>(TEXT("UIExtension.UIExtensionPointWidget class is unavailable; extension point detection falls back to reflected ExtensionPointTag properties.")));
    }
    if (bIncludeLayerCandidates && !ActivatableContainerClass)
    {
        Warnings.Add(MakeShared<FJsonValueString>(TEXT("CommonUI.CommonActivatableWidgetContainerBase class is unavailable; layer candidate detection is disabled.")));
    }

    if (bIncludeExtensionPoints)
    {
        TArray<TSharedPtr<FJsonValue>> ExtensionPoints;
        ExtensionPoints.Reserve(Widgets.Num());
        for (UWidget* Widget : Widgets)
        {
            if (!Widget)
            {
                continue;
            }

            const bool bIsExtensionWidget = ExtensionPointWidgetClass && Widget->IsA(ExtensionPointWidgetClass);
            const bool bHasExtensionTag = HasGameplayTagProperty(Widget, TEXT("ExtensionPointTag"));
            if (bIsExtensionWidget || bHasExtensionTag)
            {
                ExtensionPoints.Add(MakeShared<FJsonValueObject>(ExtensionPointSummary(Widget)));
            }
        }
        Result->SetArrayField(TEXT("extension_points"), ExtensionPoints);
    }

    if (bIncludeLayerCandidates)
    {
        TArray<TSharedPtr<FJsonValue>> LayerCandidates;
        LayerCandidates.Reserve(Widgets.Num());
        if (ActivatableContainerClass)
        {
            for (UWidget* Widget : Widgets)
            {
                if (Widget && Widget->IsA(ActivatableContainerClass))
                {
                    LayerCandidates.Add(MakeShared<FJsonValueObject>(WidgetSummary(Widget)));
                }
            }
        }
        Result->SetArrayField(TEXT("layer_candidates"), LayerCandidates);
    }

    if (bIncludeWidgetTree)
    {
        TArray<TSharedPtr<FJsonValue>> WidgetSummaries;
        for (UWidget* Widget : Widgets)
        {
            if (Widget)
            {
                WidgetSummaries.Add(MakeShared<FJsonValueObject>(WidgetSummary(Widget)));
            }
        }
        Result->SetArrayField(TEXT("widgets"), WidgetSummaries);
    }

    Result->SetArrayField(TEXT("warnings"), Warnings);
    return FMonolithActionResult::Success(Result);
}

FMonolithActionResult FMonolithUIActions::HandleDescribeCommonMessagingFlow(const TSharedPtr<FJsonObject>& Params)
{
    using namespace MonolithUICommonFrameworkInternal;

    UClass* MessagingBaseClass = LoadClassPath(TEXT("/Script/CommonGame.CommonMessagingSubsystem"));
    UClass* DialogBaseClass = LoadClassPath(TEXT("/Script/CommonGame.CommonGameDialog"));
    UClass* DialogDescriptorClass = LoadClassPath(TEXT("/Script/CommonGame.CommonGameDialogDescriptor"));
    UClass* PrimaryGameLayoutClass = LoadClassPath(TEXT("/Script/CommonGame.PrimaryGameLayout"));

    FString RequestedMessagingClass;
    UClass* MessagingClass = ResolveMessagingClass(Params, MessagingBaseClass, RequestedMessagingClass);
    const FString ConfigSection = ResolveMessagingConfigSection(Params, MessagingClass);
    const FString ConfirmationDialogPath = ResolveDialogClassPath(
        Params,
        TEXT("confirmation_dialog_class"),
        ConfigSection,
        TEXT("ConfirmationDialogClass"),
        MessagingClass);
    const FString ErrorDialogPath = ResolveDialogClassPath(
        Params,
        TEXT("error_dialog_class"),
        ConfigSection,
        TEXT("ErrorDialogClass"),
        MessagingClass);

    const FString ModalLayerTagName = MonolithUIInternal::GetOptionalString(Params, TEXT("modal_layer_tag"), TEXT("UI.Layer.Modal"));
    const FGameplayTag ModalLayerTag = FGameplayTag::RequestGameplayTag(FName(*ModalLayerTagName), /*ErrorIfNotFound=*/false);

    TArray<TSharedPtr<FJsonValue>> Checks;
    TArray<TSharedPtr<FJsonValue>> Issues;
    TArray<TSharedPtr<FJsonValue>> Warnings;

    AddCheck(Checks, TEXT("common_messaging_subsystem_available"), MessagingBaseClass != nullptr, MessagingBaseClass ? TEXT("ok") : TEXT("failed"), ClassPath(MessagingBaseClass));
    AddCheck(Checks, TEXT("common_game_dialog_available"), DialogBaseClass != nullptr, DialogBaseClass ? TEXT("ok") : TEXT("failed"), ClassPath(DialogBaseClass));
    AddCheck(Checks, TEXT("common_game_dialog_descriptor_available"), DialogDescriptorClass != nullptr, DialogDescriptorClass ? TEXT("ok") : TEXT("failed"), ClassPath(DialogDescriptorClass));
    AddCheck(Checks, TEXT("primary_game_layout_available"), PrimaryGameLayoutClass != nullptr, PrimaryGameLayoutClass ? TEXT("ok") : TEXT("failed"), ClassPath(PrimaryGameLayoutClass));
    AddCheck(Checks, TEXT("modal_layer_tag_registered"), ModalLayerTag.IsValid(), ModalLayerTag.IsValid() ? TEXT("ok") : TEXT("failed"), ModalLayerTagName);

    if (!MessagingBaseClass)
    {
        AddIssue(Issues, TEXT("common_messaging_subsystem_unavailable"), TEXT("CommonGame.CommonMessagingSubsystem is unavailable."));
    }
    if (!DialogBaseClass)
    {
        AddIssue(Issues, TEXT("common_game_dialog_unavailable"), TEXT("CommonGame.CommonGameDialog is unavailable."));
    }
    if (!MessagingClass)
    {
        AddIssue(Issues, TEXT("messaging_class_not_found"), FString::Printf(TEXT("Messaging class '%s' could not be loaded."), *RequestedMessagingClass));
    }
    else if (MessagingBaseClass && !MessagingClass->IsChildOf(MessagingBaseClass))
    {
        AddIssue(Issues, TEXT("messaging_class_wrong_parent"), FString::Printf(TEXT("Messaging class '%s' is not a CommonMessagingSubsystem subclass."), *MessagingClass->GetPathName()));
    }
    if (!ModalLayerTag.IsValid())
    {
        AddIssue(Issues, TEXT("modal_layer_tag_not_registered"), FString::Printf(TEXT("GameplayTag '%s' is not registered."), *ModalLayerTagName));
    }
    if (ConfirmationDialogPath.IsEmpty())
    {
        AddIssue(Warnings, TEXT("confirmation_dialog_class_missing"), TEXT("ConfirmationDialogClass is not configured."), TEXT("warning"));
    }
    if (ErrorDialogPath.IsEmpty())
    {
        AddIssue(Warnings, TEXT("error_dialog_class_missing"), TEXT("ErrorDialogClass is not configured."), TEXT("warning"));
    }

    const bool bIncludeSubclasses = MonolithUIInternal::GetOptionalBool(Params, TEXT("include_subclasses"), true);
    const int32 SubclassLimit = GetOptionalInt(Params, TEXT("subclass_limit"), 40, 1, 200);

    TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
    Result->SetStringField(TEXT("action"), TEXT("describe_common_messaging_flow"));
    Result->SetBoolField(TEXT("common_game_available"), MessagingBaseClass != nullptr && DialogBaseClass != nullptr && PrimaryGameLayoutClass != nullptr);
    Result->SetObjectField(TEXT("messaging_class"), MessagingClassSummary(MessagingClass, MessagingBaseClass, RequestedMessagingClass));
    Result->SetStringField(TEXT("config_section"), ConfigSection);
    Result->SetObjectField(TEXT("confirmation_dialog"), DialogClassSummary(TEXT("confirmation"), ConfirmationDialogPath, DialogBaseClass));
    Result->SetObjectField(TEXT("error_dialog"), DialogClassSummary(TEXT("error"), ErrorDialogPath, DialogBaseClass));
    Result->SetStringField(TEXT("modal_layer_tag"), ModalLayerTagName);
    Result->SetBoolField(TEXT("modal_layer_tag_registered"), ModalLayerTag.IsValid());
    Result->SetStringField(TEXT("push_entrypoint"), TEXT("PrimaryGameLayout.PushWidgetToLayerStack"));
    Result->SetStringField(TEXT("dialog_setup_entrypoint"), TEXT("CommonGameDialog.SetupDialog"));
    Result->SetStringField(TEXT("default_ui_policy_class"), GetDefaultUIPolicyClassPath());
    if (bIncludeSubclasses)
    {
        Result->SetArrayField(TEXT("messaging_subclasses"), MessagingSubclassSummaries(MessagingBaseClass, SubclassLimit));
    }
    Result->SetArrayField(TEXT("checks"), Checks);
    Result->SetArrayField(TEXT("issues"), Issues);
    Result->SetArrayField(TEXT("warnings"), Warnings);
    Result->SetStringField(TEXT("overall_status"), Issues.Num() == 0 ? (Warnings.Num() == 0 ? TEXT("ok") : TEXT("warnings")) : TEXT("issues"));
    return FMonolithActionResult::Success(Result);
}

FMonolithActionResult FMonolithUIActions::HandleValidateCommonDialogContract(const TSharedPtr<FJsonObject>& Params)
{
    using namespace MonolithUICommonFrameworkInternal;

    UClass* MessagingBaseClass = LoadClassPath(TEXT("/Script/CommonGame.CommonMessagingSubsystem"));
    UClass* DialogBaseClass = LoadClassPath(TEXT("/Script/CommonGame.CommonGameDialog"));
    FString RequestedMessagingClass;
    UClass* MessagingClass = ResolveMessagingClass(Params, MessagingBaseClass, RequestedMessagingClass);
    const FString ConfigSection = ResolveMessagingConfigSection(Params, MessagingClass);
    const FString ConfirmationDialogPath = ResolveDialogClassPath(
        Params,
        TEXT("confirmation_dialog_class"),
        ConfigSection,
        TEXT("ConfirmationDialogClass"),
        MessagingClass);
    const FString ErrorDialogPath = ResolveDialogClassPath(
        Params,
        TEXT("error_dialog_class"),
        ConfigSection,
        TEXT("ErrorDialogClass"),
        MessagingClass);

    TArray<TSharedPtr<FJsonValue>> Checks;
    TArray<TSharedPtr<FJsonValue>> Issues;

    AddCheck(Checks, TEXT("common_messaging_subsystem_available"), MessagingBaseClass != nullptr, MessagingBaseClass ? TEXT("ok") : TEXT("failed"), ClassPath(MessagingBaseClass));
    AddCheck(Checks, TEXT("common_game_dialog_available"), DialogBaseClass != nullptr, DialogBaseClass ? TEXT("ok") : TEXT("failed"), ClassPath(DialogBaseClass));
    AddCheck(
        Checks,
        TEXT("messaging_class_child_of_common_messaging_subsystem"),
        MessagingClass && MessagingBaseClass && MessagingClass->IsChildOf(MessagingBaseClass),
        (MessagingClass && MessagingBaseClass && MessagingClass->IsChildOf(MessagingBaseClass)) ? TEXT("ok") : TEXT("failed"),
        MessagingClass ? MessagingClass->GetPathName() : RequestedMessagingClass);

    if (!MessagingBaseClass)
    {
        AddIssue(Issues, TEXT("common_messaging_subsystem_unavailable"), TEXT("CommonGame.CommonMessagingSubsystem is unavailable."));
    }
    if (!DialogBaseClass)
    {
        AddIssue(Issues, TEXT("common_game_dialog_unavailable"), TEXT("CommonGame.CommonGameDialog is unavailable."));
    }
    if (!MessagingClass)
    {
        AddIssue(Issues, TEXT("messaging_class_not_found"), FString::Printf(TEXT("Messaging class '%s' could not be loaded."), *RequestedMessagingClass));
    }
    else if (MessagingBaseClass && !MessagingClass->IsChildOf(MessagingBaseClass))
    {
        AddIssue(Issues, TEXT("messaging_class_wrong_parent"), FString::Printf(TEXT("Messaging class '%s' is not a CommonMessagingSubsystem subclass."), *MessagingClass->GetPathName()));
    }

    TSharedPtr<FJsonObject> ConfirmationDialog = DialogClassSummary(TEXT("confirmation"), ConfirmationDialogPath, DialogBaseClass);
    TSharedPtr<FJsonObject> ErrorDialog = DialogClassSummary(TEXT("error"), ErrorDialogPath, DialogBaseClass);
    AddDialogContractIssues(TEXT("confirmation"), ConfirmationDialog, Issues);
    AddDialogContractIssues(TEXT("error"), ErrorDialog, Issues);

    TArray<TSharedPtr<FJsonValue>> Dialogs;
    Dialogs.Add(MakeShared<FJsonValueObject>(ConfirmationDialog));
    Dialogs.Add(MakeShared<FJsonValueObject>(ErrorDialog));

    TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
    Result->SetStringField(TEXT("action"), TEXT("validate_common_dialog_contract"));
    Result->SetBoolField(TEXT("ok"), Issues.Num() == 0);
    Result->SetObjectField(TEXT("messaging_class"), MessagingClassSummary(MessagingClass, MessagingBaseClass, RequestedMessagingClass));
    Result->SetStringField(TEXT("config_section"), ConfigSection);
    Result->SetArrayField(TEXT("dialogs"), Dialogs);
    Result->SetArrayField(TEXT("checks"), Checks);
    Result->SetArrayField(TEXT("issues"), Issues);
    return FMonolithActionResult::Success(Result);
}

FMonolithActionResult FMonolithUIActions::HandleValidateCommonLayerPushContract(const TSharedPtr<FJsonObject>& Params)
{
    using namespace MonolithUICommonFrameworkInternal;

    const bool bRequireLayoutAsset = MonolithUIInternal::GetOptionalBool(Params, TEXT("require_layout_asset"), false);
    const FString LayoutAssetPath = MonolithUIInternal::GetOptionalString(Params, TEXT("layout_asset_path"));
    if (bRequireLayoutAsset && LayoutAssetPath.IsEmpty())
    {
        return FMonolithActionResult::Error(
            TEXT("validate_common_layer_push_contract: require_layout_asset=true requires layout_asset_path."),
            FMonolithJsonUtils::ErrInvalidParams);
    }

    UClass* PrimaryGameLayoutClass = LoadClassPath(TEXT("/Script/CommonGame.PrimaryGameLayout"));
    UClass* ContainerBaseClass = LoadClassPath(TEXT("/Script/CommonUI.CommonActivatableWidgetContainerBase"));
    UClass* DialogBaseClass = LoadClassPath(TEXT("/Script/CommonGame.CommonGameDialog"));
    UClass* MessagingBaseClass = LoadClassPath(TEXT("/Script/CommonGame.CommonMessagingSubsystem"));

    const FString LayerTagName = MonolithUIInternal::GetOptionalString(Params, TEXT("layer_tag"), TEXT("UI.Layer.Modal"));
    const FGameplayTag LayerTag = FGameplayTag::RequestGameplayTag(FName(*LayerTagName), /*ErrorIfNotFound=*/false);
    const FString LayerWidgetName = MonolithUIInternal::GetOptionalString(Params, TEXT("layer_widget_name"));

    FString DialogClassPath = MonolithUIInternal::GetOptionalString(Params, TEXT("dialog_class"));
    if (DialogClassPath.IsEmpty())
    {
        FString RequestedMessagingClass;
        UClass* MessagingClass = ResolveMessagingClass(Params, MessagingBaseClass, RequestedMessagingClass);
        const FString ConfigSection = ResolveMessagingConfigSection(Params, MessagingClass);
        DialogClassPath = ResolveDialogClassPath(
            Params,
            TEXT("confirmation_dialog_class"),
            ConfigSection,
            TEXT("ConfirmationDialogClass"),
            MessagingClass);
    }

    TArray<TSharedPtr<FJsonValue>> Checks;
    TArray<TSharedPtr<FJsonValue>> Issues;
    TArray<TSharedPtr<FJsonValue>> Warnings;

    AddCheck(Checks, TEXT("primary_game_layout_available"), PrimaryGameLayoutClass != nullptr, PrimaryGameLayoutClass ? TEXT("ok") : TEXT("failed"), ClassPath(PrimaryGameLayoutClass));
    AddCheck(Checks, TEXT("common_activatable_container_available"), ContainerBaseClass != nullptr, ContainerBaseClass ? TEXT("ok") : TEXT("failed"), ClassPath(ContainerBaseClass));
    AddCheck(Checks, TEXT("common_game_dialog_available"), DialogBaseClass != nullptr, DialogBaseClass ? TEXT("ok") : TEXT("failed"), ClassPath(DialogBaseClass));
    AddCheck(Checks, TEXT("layer_tag_registered"), LayerTag.IsValid(), LayerTag.IsValid() ? TEXT("ok") : TEXT("failed"), LayerTagName);

    if (!PrimaryGameLayoutClass)
    {
        AddIssue(Issues, TEXT("primary_game_layout_unavailable"), TEXT("CommonGame.PrimaryGameLayout is unavailable."));
    }
    if (!ContainerBaseClass)
    {
        AddIssue(Issues, TEXT("common_activatable_container_unavailable"), TEXT("CommonUI.CommonActivatableWidgetContainerBase is unavailable."));
    }
    if (!DialogBaseClass)
    {
        AddIssue(Issues, TEXT("common_game_dialog_unavailable"), TEXT("CommonGame.CommonGameDialog is unavailable."));
    }
    if (!LayerTag.IsValid())
    {
        AddIssue(Issues, TEXT("layer_tag_not_registered"), FString::Printf(TEXT("GameplayTag '%s' is not registered."), *LayerTagName));
    }

    TSharedPtr<FJsonObject> Dialog = DialogClassSummary(TEXT("dialog"), DialogClassPath, DialogBaseClass);
    AddDialogContractIssues(TEXT("dialog"), Dialog, Issues);

    TSharedPtr<FJsonObject> Layout = MakeShared<FJsonObject>();
    Layout->SetStringField(TEXT("asset_path"), LayoutAssetPath);
    Layout->SetBoolField(TEXT("provided"), !LayoutAssetPath.IsEmpty());
    Layout->SetStringField(TEXT("requested_layer_widget_name"), LayerWidgetName);
    Layout->SetStringField(TEXT("register_layer_proof_status"), LayoutAssetPath.IsEmpty() ? TEXT("layout_asset_not_supplied") : TEXT("not_evaluated"));
    Layout->SetBoolField(TEXT("register_layer_function_found"), PrimaryGameLayoutClass && PrimaryGameLayoutClass->FindFunctionByName(FName(TEXT("RegisterLayer"))) != nullptr);

    if (LayoutAssetPath.IsEmpty())
    {
        AddIssue(Warnings, TEXT("layout_asset_not_supplied"), TEXT("No PrimaryGameLayout Widget Blueprint was supplied; layer container and RegisterLayer evidence were not inspected."), TEXT("warning"));
    }
    else
    {
        FMonolithActionResult Err;
        UWidgetBlueprint* WBP = MonolithUIInternal::LoadWidgetBlueprint(LayoutAssetPath, Err);
        if (!WBP)
        {
            return Err;
        }
        if (!WBP->WidgetTree)
        {
            return FMonolithActionResult::Error(TEXT("WidgetTree is null (editor-only data not available)"));
        }

        Layout->SetStringField(TEXT("parent_class"), WBP->ParentClass ? WBP->ParentClass->GetName() : FString());
        Layout->SetStringField(TEXT("parent_class_path"), WBP->ParentClass ? WBP->ParentClass->GetPathName() : FString());
        const bool bIsPrimaryGameLayout = WBP->ParentClass && PrimaryGameLayoutClass && WBP->ParentClass->IsChildOf(PrimaryGameLayoutClass);
        Layout->SetBoolField(TEXT("is_primary_game_layout"), bIsPrimaryGameLayout);
        if (!bIsPrimaryGameLayout)
        {
            AddIssue(
                Issues,
                TEXT("layout_wrong_parent"),
                FString::Printf(
                    TEXT("Layout asset '%s' parent '%s' is not a CommonGame.PrimaryGameLayout subclass."),
                    *LayoutAssetPath,
                    WBP->ParentClass ? *WBP->ParentClass->GetPathName() : TEXT("<null>")));
        }

        TArray<UWidget*> Widgets;
        WBP->WidgetTree->GetAllWidgets(Widgets);
        Layout->SetNumberField(TEXT("widget_count"), Widgets.Num());

        TArray<TSharedPtr<FJsonValue>> LayerCandidates;
        LayerCandidates.Reserve(Widgets.Num());
        bool bExpectedLayerWidgetFound = false;
        if (ContainerBaseClass)
        {
            for (UWidget* Widget : Widgets)
            {
                if (!Widget || !Widget->IsA(ContainerBaseClass))
                {
                    continue;
                }

                TSharedPtr<FJsonObject> Candidate = WidgetSummary(Widget);
                const bool bNameMatches = !LayerWidgetName.IsEmpty() && Widget->GetName() == LayerWidgetName;
                Candidate->SetBoolField(TEXT("matches_requested_layer_widget_name"), bNameMatches);
                bExpectedLayerWidgetFound = bExpectedLayerWidgetFound || bNameMatches;
                LayerCandidates.Add(MakeShared<FJsonValueObject>(Candidate));
            }
        }

        Layout->SetArrayField(TEXT("layer_candidates"), LayerCandidates);
        Layout->SetNumberField(TEXT("layer_candidate_count"), LayerCandidates.Num());
        Layout->SetBoolField(TEXT("requested_layer_widget_found"), LayerWidgetName.IsEmpty() ? LayerCandidates.Num() > 0 : bExpectedLayerWidgetFound);

        if (!LayerWidgetName.IsEmpty() && !bExpectedLayerWidgetFound)
        {
            AddIssue(Issues, TEXT("layer_widget_not_found"), FString::Printf(TEXT("Layer widget '%s' was not found as a CommonActivatableWidgetContainerBase candidate."), *LayerWidgetName));
            Layout->SetStringField(TEXT("register_layer_proof_status"), TEXT("missing_requested_container_candidate"));
        }
        else if (LayerCandidates.Num() == 0)
        {
            AddIssue(Issues, TEXT("layer_container_not_found"), TEXT("No CommonActivatableWidgetContainerBase layer candidate was found in the layout WBP."));
            Layout->SetStringField(TEXT("register_layer_proof_status"), TEXT("missing_container_candidate"));
        }
        else
        {
            Layout->SetStringField(TEXT("register_layer_proof_status"), TEXT("container_candidate_found_graph_wiring_not_proven"));
            AddIssue(
                Warnings,
                TEXT("register_layer_graph_not_proven"),
                TEXT("A layer container candidate exists, but this read-only validator does not prove the layout graph/code calls RegisterLayer with the requested tag and widget."),
                TEXT("warning"));
        }
    }

    TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
    Result->SetStringField(TEXT("action"), TEXT("validate_common_layer_push_contract"));
    Result->SetBoolField(TEXT("ok"), Issues.Num() == 0);
    Result->SetStringField(TEXT("layer_tag"), LayerTagName);
    Result->SetBoolField(TEXT("layer_tag_registered"), LayerTag.IsValid());
    Result->SetObjectField(TEXT("dialog"), Dialog);
    Result->SetObjectField(TEXT("layout"), Layout);
    Result->SetArrayField(TEXT("checks"), Checks);
    Result->SetArrayField(TEXT("issues"), Issues);
    Result->SetArrayField(TEXT("warnings"), Warnings);
    return FMonolithActionResult::Success(Result);
}

// --- remove_widget ---
FMonolithActionResult FMonolithUIActions::HandleRemoveWidget(const TSharedPtr<FJsonObject>& Params)
{
    FMonolithActionResult ParamError;
    FString AssetPath;
    if (!MonolithUIInternal::TryGetRequiredString(Params, TEXT("asset_path"), AssetPath, ParamError))
        return ParamError;
    FString WidgetName;
    if (!MonolithUIInternal::TryGetRequiredString(Params, TEXT("widget_name"), WidgetName, ParamError))
        return ParamError;

    FMonolithActionResult Err;
    UWidgetBlueprint* WBP = MonolithUIInternal::LoadWidgetBlueprint(AssetPath, Err);
    if (!WBP) return Err;

    UWidget* Widget = WBP->WidgetTree->FindWidget(FName(*WidgetName));
    if (!Widget)
    {
        return FMonolithActionResult::Error(
            FString::Printf(TEXT("Widget '%s' not found in widget tree"), *WidgetName));
    }

    // Cannot remove root
    if (Widget == WBP->WidgetTree->RootWidget)
    {
        return FMonolithActionResult::Error(TEXT("Cannot remove the root widget"));
    }

    TSet<UWidget*> WidgetsToDelete;
    WidgetsToDelete.Add(Widget);
    FWidgetBlueprintEditorUtils::DeleteWidgets(WBP, WidgetsToDelete, FWidgetBlueprintEditorUtils::EDeleteWidgetWarningType::DeleteSilently);

    FBlueprintEditorUtils::MarkBlueprintAsStructurallyModified(WBP);

    bool bCompile = true;
    bCompile = MonolithUIInternal::GetOptionalBool(Params, TEXT("compile"), true);
    if (bCompile) FKismetEditorUtilities::CompileBlueprint(WBP);

    TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
    Result->SetStringField(TEXT("removed"), WidgetName);
    Result->SetBoolField(TEXT("compiled"), bCompile);
    return FMonolithActionResult::Success(Result);
}

// --- set_widget_property ---
//
// Phase C rewrite (2026-04-26): the handler now routes through
// FUIReflectionHelper. Default mode (`raw_mode=false`) gates the write through
// the per-type curated allowlist on FUIPropertyAllowlist; `raw_mode=true`
// preserves the legacy bare-FProperty::ImportText_Direct path so any existing
// caller that previously wrote arbitrary properties unconditionally keeps
// working by adding `raw_mode=true` to its parameter dictionary.
//
// Value handling: the action schema declares `value` as type "string", but
// the wire payload is a TSharedPtr<FJsonValue> — callers can supply numbers,
// booleans, arrays, objects, and the helper dispatches on FProperty kind.
// We grab the field via TryGetField (not GetStringField) so non-string JSON
// shapes survive.
FMonolithActionResult FMonolithUIActions::HandleSetWidgetProperty(const TSharedPtr<FJsonObject>& Params)
{
    if (!Params.IsValid())
    {
        return FMonolithActionResult::Error(TEXT("set_widget_property: Params is null"));
    }

    FMonolithActionResult ParamError;
    FString AssetPath;
    if (!MonolithUIInternal::TryGetRequiredString(Params, TEXT("asset_path"), AssetPath, ParamError))
        return ParamError;
    FString WidgetName;
    if (!MonolithUIInternal::TryGetRequiredString(Params, TEXT("widget_name"), WidgetName, ParamError))
        return ParamError;
    FString PropertyName;
    if (!MonolithUIInternal::TryGetRequiredString(Params, TEXT("property_name"), PropertyName, ParamError))
        return ParamError;
    const bool bRawMode = MonolithUIInternal::GetOptionalBool(Params, TEXT("raw_mode"), false);

    // Bug #6 fix (2026-05-16 UI gap audit): accept BOTH `value` and
    // `property_value` as aliases. Discovery output's schema description
    // historically left the param name ambiguous; some callers probed
    // with `property_value` and got
    // "Missing required param" followed by a SECOND error about wbp_path
    // when the param was renamed — the dual-failure mode wasted calls. We
    // now accept either spelling and surface a single coherent error that
    // names both forms AND preserves wbp_path in the message.
    //
    // Pull as the raw JSON value so non-string shapes (numbers, booleans,
    // arrays, struct objects) survive — FUIReflectionHelper dispatches on
    // FProperty kind, not on FString shape.
    TSharedPtr<FJsonValue> ValueJson = Params->TryGetField(TEXT("value"));
    if (!ValueJson.IsValid())
    {
        ValueJson = Params->TryGetField(TEXT("property_value"));
    }
    if (!ValueJson.IsValid())
    {
        return FMonolithActionResult::Error(FString::Printf(
            TEXT("set_widget_property: missing required param 'value' (alias: 'property_value') on wbp_path='%s', widget_name='%s', property_name='%s'"),
            *AssetPath, *WidgetName, *PropertyName));
    }

    FMonolithActionResult Err;
    UWidgetBlueprint* WBP = MonolithUIInternal::LoadWidgetBlueprint(AssetPath, Err);
    if (!WBP) return Err;

    UWidget* Widget = WBP->WidgetTree->FindWidget(FName(*WidgetName));
    if (!Widget)
    {
        // Phase K — Lookup error. valid_options is intentionally empty (would
        // require scanning the live WidgetTree, which the LLM can do via
        // ui::get_widget_tree as the suggested_fix indicates).
        return MonolithUIInternal::MakeErrorFromSpecError(MonolithUIInternal::MakeSpecError(
            TEXT("Lookup"),
            TEXT("/widget_name"),
            FString::Printf(TEXT("Widget '%s' not found in WBP '%s'."), *WidgetName, *AssetPath),
            TEXT("Call ui::get_widget_tree to enumerate live widget names.")));
    }

    if (MonolithUISetWidgetPropertyInternal::IsVariableFlagProperty(PropertyName))
    {
        bool bIsVariable = false;
        FString BoolParseError;
        if (!MonolithUISetWidgetPropertyInternal::TryReadBoolValue(ValueJson, bIsVariable, BoolParseError))
        {
            return FMonolithActionResult::Error(FString::Printf(
                TEXT("set_widget_property: property '%s' routes to ui.set_widget_is_variable and requires a boolean-compatible value (%s)."),
                *PropertyName,
                *BoolParseError),
                -32602);
        }

        const bool bWasVariable = Widget->bIsVariable;

        Widget->Modify();
        Widget->bIsVariable = bIsVariable;

        FBlueprintEditorUtils::MarkBlueprintAsStructurallyModified(WBP);
        FKismetEditorUtilities::CompileBlueprint(WBP);
        WBP->GetOutermost()->MarkPackageDirty();

        TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
        Result->SetStringField(TEXT("widget"), WidgetName);
        Result->SetStringField(TEXT("widget_name"), WidgetName);
        Result->SetStringField(TEXT("property"), PropertyName);
        Result->SetStringField(TEXT("value"), bIsVariable ? TEXT("true") : TEXT("false"));
        Result->SetBoolField(TEXT("is_variable"), bIsVariable);
        Result->SetBoolField(TEXT("changed"), bWasVariable != bIsVariable);
        Result->SetBoolField(TEXT("compiled"), true);
        Result->SetBoolField(TEXT("raw_mode"), bRawMode);
        Result->SetStringField(TEXT("routed_action"), TEXT("ui.set_widget_is_variable"));
        return FMonolithActionResult::Success(Result);
    }

    // ----- Phase C primary path: gated reflection helper -----
    UMonolithUIRegistrySubsystem* Sub = UMonolithUIRegistrySubsystem::Get();
    FUIPropertyPathCache* Cache = Sub ? Sub->GetPathCache() : nullptr;
    const FUIPropertyAllowlist* Allowlist = Sub ? &Sub->GetAllowlist() : nullptr;

    FUIReflectionHelper Helper(Cache, Allowlist);
    const FUIReflectionApplyResult ApplyRes = Helper.Apply(Widget, PropertyName, ValueJson, bRawMode);

    if (ApplyRes.bSuccess)
    {
        Widget->SynchronizeProperties();
        FBlueprintEditorUtils::MarkBlueprintAsModified(WBP);

        const bool bCompile = MonolithUIInternal::GetOptionalBool(Params, TEXT("compile"), false);
        if (bCompile) FKismetEditorUtilities::CompileBlueprint(WBP);

        TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
        Result->SetStringField(TEXT("widget"), WidgetName);
        Result->SetStringField(TEXT("property"), PropertyName);
        // Echo the value as its string serialisation for compat with existing
        // call-site expectations (legacy result included a `value` string).
        Result->SetStringField(TEXT("value"), ValueJson->AsString());
        Result->SetBoolField(TEXT("compiled"), bCompile);
        Result->SetBoolField(TEXT("raw_mode"), bRawMode);
        return FMonolithActionResult::Success(Result);
    }

    // ----- Failure path: propagate the helper's structured reason -----
    //
    // Phase K: replaced the prior `| reason=X detail=Y` interim shape with the
    // FUISpecError formatter so the payload now carries category / json_path /
    // suggested_fix / valid_options on the same key:value rails as the
    // `build_ui_from_spec` validation block. For NotInAllowlist failures we
    // populate valid_options with the actual allowlist entries for this
    // widget type so the LLM can pick a legal path on its next attempt.
    FString SuggestedFix;
    TArray<FString> ValidOptions;
    FName Category = TEXT("Property");

    if (ApplyRes.FailureReason == TEXT("NotInAllowlist"))
    {
        Category = TEXT("Allowlist");
        SuggestedFix = TEXT("Path not on the curated per-type allowlist. Pick a path from valid_options, or pass raw_mode=true to bypass the gate (legacy compat).");
        if (Allowlist)
        {
            // Pull the live allowlist for this widget type. The list can be
            // empty (registry not yet populated, type not on the allowlist):
            // in that case suggested_fix still names raw_mode as the escape.
            const FName Token = FName(*Widget->GetClass()->GetName());
            ValidOptions = Allowlist->GetAllowedPaths(Token);
        }
    }
    else if (ApplyRes.FailureReason == TEXT("PropertyNotFound"))
    {
        Category = TEXT("Property");
        SuggestedFix = TEXT("Property not found via reflection on the widget class. Verify the property name spelling and walk through any FStructProperty hops with dotted segments (e.g. 'Padding.Left').");
    }
    else if (ApplyRes.FailureReason == TEXT("ParseFailed"))
    {
        Category = TEXT("ValueParse");
        SuggestedFix = TEXT("Could not parse the value into the property's struct/scalar shape. Check the FProperty kind (Color/Vector2D/Margin/enum) and supply the matching JSON literal or struct.");
    }
    else if (ApplyRes.FailureReason == TEXT("TypeMismatch"))
    {
        Category = TEXT("TypeMismatch");
        SuggestedFix = TEXT("Value JSON shape doesn't match the FProperty's expected type. See ApplyRes.Detail for the expected kind (e.g. 'expected number', 'expected bool').");
    }
    else
    {
        SuggestedFix = TEXT("Unknown failure mode. Check the editor log for ApplyRes.Detail context.");
    }

    FUISpecError E = MonolithUIInternal::MakeSpecError(
        Category,
        FString::Printf(TEXT("/property_name (%s)"), *PropertyName),
        FString::Printf(TEXT("set_widget_property failed: %s on %s.%s (%s)"),
            *ApplyRes.FailureReason,
            *Widget->GetClass()->GetName(),
            *PropertyName,
            *ApplyRes.Detail),
        SuggestedFix,
        MoveTemp(ValidOptions));
    E.WidgetId = FName(*WidgetName);
    // -32602 is JSON-RPC "invalid params" — gate-rejection is caller-input,
    // not internal-error.
    return MonolithUIInternal::MakeErrorFromSpecError(E, -32602);
}

// --- compile_widget ---
// Bug #5 fix (2026-05-16 UI gap audit): the action now always returns
// errors[] + warnings[] + notes[] arrays harvested from FCompilerResultsLog.
// The shape mirrors blueprint_query("compile_blueprint") so callers can use
// a single parser. Pattern mirrored from
// MonolithBlueprintCompileActions.cpp:80 (HandleCompileBlueprint).
FMonolithActionResult FMonolithUIActions::HandleCompileWidget(const TSharedPtr<FJsonObject>& Params)
{
    FMonolithActionResult ParamError;
    FString AssetPath;
    if (!MonolithUIInternal::TryGetRequiredString(Params, TEXT("asset_path"), AssetPath, ParamError))
        return ParamError;
    FMonolithActionResult Err;
    UWidgetBlueprint* WBP = MonolithUIInternal::LoadWidgetBlueprint(AssetPath, Err);
    if (!WBP) return Err;

    // Drive the compile through the FCompilerResultsLog-capturing overload so
    // the Messages array carries every Tokenized diagnostic the validator and
    // the K2 compiler emit. SkipGarbageCollection matches the blueprint_query
    // flow's flags and keeps the action interactive-fast.
    FCompilerResultsLog Results;
    FKismetEditorUtilities::CompileBlueprint(WBP, EBlueprintCompileOptions::SkipGarbageCollection, &Results);

    TArray<TSharedPtr<FJsonValue>> ErrorArr;
    TArray<TSharedPtr<FJsonValue>> WarnArr;
    TArray<TSharedPtr<FJsonValue>> NoteArr;
    ErrorArr.Reserve(Results.Messages.Num());
    WarnArr.Reserve(Results.Messages.Num());
    NoteArr.Reserve(Results.Messages.Num());
    for (const TSharedRef<FTokenizedMessage>& Msg : Results.Messages)
    {
        TSharedPtr<FJsonObject> MsgObj = MakeShared<FJsonObject>();
        MsgObj->SetStringField(TEXT("message"), Msg->ToText().ToString());

        const EMessageSeverity::Type Sev = Msg->GetSeverity();
        if (Sev == EMessageSeverity::Error)
        {
            ErrorArr.Add(MakeShared<FJsonValueObject>(MsgObj));
        }
        else if (Sev == EMessageSeverity::Warning)
        {
            WarnArr.Add(MakeShared<FJsonValueObject>(MsgObj));
        }
        else
        {
            // Info / PerformanceWarning / unknown all surface as notes so
            // callers see them without conflating with hard errors.
            NoteArr.Add(MakeShared<FJsonValueObject>(MsgObj));
        }
    }

    // Status-string mapping shared across the two response paths (error vs
    // success). Keep aligned with blueprint_query("compile_blueprint") so
    // callers can switch on the same set of tokens.
    FString StatusStr;
    switch (WBP->Status)
    {
    case BS_Unknown:              StatusStr = TEXT("unknown"); break;
    case BS_Dirty:                StatusStr = TEXT("dirty"); break;
    case BS_Error:                StatusStr = TEXT("error"); break;
    case BS_UpToDate:             StatusStr = TEXT("up_to_date"); break;
    case BS_UpToDateWithWarnings: StatusStr = TEXT("up_to_date_with_warnings"); break;
    case BS_BeingCreated:         StatusStr = TEXT("being_created"); break;
    default:                      StatusStr = TEXT("other"); break;
    }

    // Phase K — when the compiler reports BS_Error, surface that as an
    // FUISpecError-shaped failure rather than a success-with-status=error.
    // The LLM consumer can branch cleanly on bSuccess instead of having to
    // parse a string status field from a "successful" call. Bug #5 evolution:
    // append the FCompilerResultsLog ValidOptions list with the verbatim
    // error/warning messages — the dispatcher (MonolithHttpServer:716)
    // surfaces only the FMonolithActionResult::ErrorMessage text on a failed
    // call, so we pack the diagnostic surface INTO that text via the
    // FUISpecError ValidOptions field (which ToLLMReport() renders as a
    // labelled `valid_options:` block in the error body).
    if (WBP->Status == BS_Error)
    {
        // Compose the ValidOptions list as "[Error] <msg>" / "[Warn] <msg>"
        // strings so the LLM sees both severity AND text without having to
        // parse a nested JSON object inside an error string.
        TArray<FString> Diagnostics;
        Diagnostics.Reserve(ErrorArr.Num() + WarnArr.Num());
        for (const TSharedPtr<FJsonValue>& V : ErrorArr)
        {
            const TSharedPtr<FJsonObject> Obj = V.IsValid() ? V->AsObject() : nullptr;
            FString MsgText;
            if (Obj.IsValid()) Obj->TryGetStringField(TEXT("message"), MsgText);
            Diagnostics.Add(FString::Printf(TEXT("[Error] %s"), *MsgText));
        }
        for (const TSharedPtr<FJsonValue>& V : WarnArr)
        {
            const TSharedPtr<FJsonObject> Obj = V.IsValid() ? V->AsObject() : nullptr;
            FString MsgText;
            if (Obj.IsValid()) Obj->TryGetStringField(TEXT("message"), MsgText);
            Diagnostics.Add(FString::Printf(TEXT("[Warn] %s"), *MsgText));
        }

        // Build a compact suggested_fix that names the first error message
        // verbatim so callers don't have to dig into valid_options[] for
        // the basic "what went wrong" answer.
        FString FirstErrorPreview;
        if (ErrorArr.Num() > 0)
        {
            const TSharedPtr<FJsonObject> Obj = ErrorArr[0]->AsObject();
            if (Obj.IsValid()) Obj->TryGetStringField(TEXT("message"), FirstErrorPreview);
        }
        const FString FailDetail = FirstErrorPreview.IsEmpty()
            ? FString::Printf(TEXT("Blueprint '%s' compiled with errors (BS_Error). See valid_options[] for diagnostics."), *AssetPath)
            : FString::Printf(TEXT("Blueprint '%s' compiled with errors (BS_Error): %s"), *AssetPath, *FirstErrorPreview);

        FUISpecError E = MonolithUIInternal::MakeSpecError(
            TEXT("Compile"),
            TEXT("/asset_path"),
            FailDetail,
            TEXT("Inspect valid_options[] in this error for the full FCompilerResultsLog surface, or call blueprint_query::compile_blueprint for the same diagnostics with per-node error linkage."),
            MoveTemp(Diagnostics));
        return MonolithUIInternal::MakeErrorFromSpecError(E, -32603);
    }

    TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
    Result->SetStringField(TEXT("asset_path"), AssetPath);
    Result->SetBoolField(TEXT("compiled"), true);
    Result->SetStringField(TEXT("status"), StatusStr);
    Result->SetArrayField(TEXT("errors"), ErrorArr);
    Result->SetArrayField(TEXT("warnings"), WarnArr);
    Result->SetArrayField(TEXT("notes"), NoteArr);
    Result->SetNumberField(TEXT("error_count"), ErrorArr.Num());
    Result->SetNumberField(TEXT("warning_count"), WarnArr.Num());
    return FMonolithActionResult::Success(Result);
}

// --- list_widget_types ---
FMonolithActionResult FMonolithUIActions::HandleListWidgetTypes(const TSharedPtr<FJsonObject>& Params)
{
    FString Filter = MonolithUIInternal::GetOptionalString(Params, TEXT("filter"));

    struct FWidgetTypeInfo
    {
        FString Name;
        FString Category;
        bool bIsPanel;
    };

    TArray<FWidgetTypeInfo> Types = {
        // Panels
        {TEXT("CanvasPanel"),       TEXT("panel"), true},
        {TEXT("VerticalBox"),       TEXT("panel"), true},
        {TEXT("HorizontalBox"),     TEXT("panel"), true},
        {TEXT("Overlay"),           TEXT("panel"), true},
        {TEXT("ScrollBox"),         TEXT("panel"), true},
        {TEXT("SizeBox"),           TEXT("panel"), true},
        {TEXT("ScaleBox"),          TEXT("panel"), true},
        {TEXT("Border"),            TEXT("panel"), true},
        {TEXT("WrapBox"),           TEXT("panel"), true},
        {TEXT("UniformGridPanel"),  TEXT("panel"), true},
        {TEXT("GridPanel"),         TEXT("panel"), true},
        {TEXT("WidgetSwitcher"),    TEXT("panel"), true},
        {TEXT("BackgroundBlur"),    TEXT("panel"), true},
        {TEXT("NamedSlot"),         TEXT("panel"), true},
        // Display
        {TEXT("TextBlock"),         TEXT("display"), false},
        {TEXT("RichTextBlock"),     TEXT("display"), false},
        {TEXT("Image"),             TEXT("display"), false},
        {TEXT("ProgressBar"),       TEXT("display"), false},
        {TEXT("Spacer"),            TEXT("layout"), false},
        // Input
        {TEXT("Button"),            TEXT("input"), true},
        {TEXT("CheckBox"),          TEXT("input"), false},
        {TEXT("Slider"),            TEXT("input"), false},
        {TEXT("EditableText"),      TEXT("input"), false},
        {TEXT("EditableTextBox"),   TEXT("input"), false},
        {TEXT("ComboBoxString"),    TEXT("input"), false},
        {TEXT("InputKeySelector"),  TEXT("input"), false},
        // Data
        {TEXT("ListView"),          TEXT("data"), true},
        {TEXT("TileView"),          TEXT("data"), true},
    };

    TArray<TSharedPtr<FJsonValue>> ResultArray;
    ResultArray.Reserve(Types.Num());
    for (const auto& T : Types)
    {
        if (!Filter.IsEmpty() && T.Category != Filter) continue;

        TSharedPtr<FJsonObject> TypeObj = MakeShared<FJsonObject>();
        TypeObj->SetStringField(TEXT("name"), T.Name);
        TypeObj->SetStringField(TEXT("category"), T.Category);
        TypeObj->SetBoolField(TEXT("is_panel"), T.bIsPanel);
        ResultArray.Add(MakeShared<FJsonValueObject>(TypeObj));
    }

    TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
    Result->SetArrayField(TEXT("widget_types"), ResultArray);
    Result->SetNumberField(TEXT("count"), ResultArray.Num());
    return FMonolithActionResult::Success(Result);
}

// =============================================================================
// Phase 2 (2026-05-16 UI gap audit) — file-static handlers
// =============================================================================
//
// Living below the class member definitions so the file's call-graph reads top
// to bottom: registration -> class statics -> Phase 2 additions. The handlers
// are file-static rather than members of FMonolithUIActions so the migration
// did not require a header change (Phase 2 §F6 prohibits new .h surface).

namespace MonolithUIActionsPhase2
{
    // ---- Phase 2 Item #7 — rename_widget --------------------------------------
    //
    // Renames a UWidget in a WBP's WidgetTree. Validates uniqueness against the
    // full tree (case-sensitive FName match) before calling Widget->Rename so
    // the rename never silently produces "Name_1" auto-suffixed via the engine's
    // collision handling — that would silently drift the caller's expected
    // identifier. Recompile through FKismetEditorUtilities::CompileBlueprint
    // matches every other mutation site in this module (e.g. HandleAddWidget,
    // HandleRemoveWidget, the CommonUI button category) for behavioural parity.

    static FMonolithActionResult HandleRenameWidget(const TSharedPtr<FJsonObject>& Params)
    {
        // Both `wbp_path` (CommonUI convention) and `asset_path` (base UMG
        // convention) are accepted — matches MonolithCommonUI::GetWbpPath.
        FString WbpPath;
        if (!Params.IsValid()
            || (!Params->TryGetStringField(TEXT("wbp_path"), WbpPath)
                && !Params->TryGetStringField(TEXT("asset_path"), WbpPath))
            || WbpPath.IsEmpty())
        {
            return FMonolithActionResult::Error(
                TEXT("wbp_path (or asset_path) required"), -32602);
        }

        FString OldName, NewName;
        FMonolithActionResult ParamError;
        if (!MonolithUIInternal::TryGetRequiredString(Params, TEXT("old_name"), OldName, ParamError))
            return ParamError;
        if (!MonolithUIInternal::TryGetRequiredString(Params, TEXT("new_name"), NewName, ParamError))
            return ParamError;

        if (OldName.Equals(NewName, ESearchCase::CaseSensitive))
        {
            return FMonolithActionResult::Error(
                TEXT("new_name is identical to old_name — nothing to do"), -32602);
        }

        FMonolithActionResult LoadErr;
        UWidgetBlueprint* WBP = MonolithUIInternal::LoadWidgetBlueprint(WbpPath, LoadErr);
        if (!WBP) return LoadErr;
        if (!WBP->WidgetTree)
        {
            return FMonolithActionResult::Error(TEXT("WidgetTree is null (editor-only data not available)"), -32603);
        }

        // Locate the target + verify uniqueness in a single tree walk. We also
        // collect any existing widget already named `new_name` so the error
        // message reports which widget is colliding.
        UWidget* Target = nullptr;
        UWidget* Collider = nullptr;
        const FName OldFName(*OldName);
        const FName NewFName(*NewName);
        WBP->WidgetTree->ForEachWidget([&](UWidget* W)
        {
            if (!W) return;
            if (W->GetFName() == OldFName) Target = W;
            if (W->GetFName() == NewFName) Collider = W;
        });

        if (!Target)
        {
            return FMonolithActionResult::Error(
                FString::Printf(TEXT("Widget '%s' not found in WBP '%s'"), *OldName, *WbpPath),
                -32602);
        }
        if (Collider)
        {
            return FMonolithActionResult::Error(
                FString::Printf(TEXT("new_name '%s' is already in use by another widget (%s) in WBP '%s'"),
                    *NewName, *Collider->GetClass()->GetName(), *WbpPath),
                -32602);
        }

        const FString OldClass = Target->GetClass()->GetName();
        const bool bWasBoundAsVariable = Target->bIsVariable;

        Target->Modify();
        WBP->Modify();

        // Rename to the same Outer (WidgetTree). REN_DontCreateRedirectors
        // keeps the package clean — no lingering linker-level redirect entry.
        // Pass `nullptr` Outer to keep the existing one (UWidget::Rename semantics).
        const bool bRenamed = Target->Rename(*NewName, nullptr, REN_DontCreateRedirectors);
        if (!bRenamed)
        {
            return FMonolithActionResult::Error(
                FString::Printf(TEXT("UObject::Rename returned false for widget '%s' -> '%s'"),
                    *OldName, *NewName),
                -32603);
        }

        // If the widget was a named variable (bIsVariable=true), the WBP holds
        // a Bindings entry and a NewVariables entry keyed on the old FName.
        // FBlueprintEditorUtils::RenameMemberVariable is the canonical fixup
        // path — it walks both arrays and replaces the FName. We only invoke
        // it when bIsVariable was set, matching the engine's own widget rename
        // path in WidgetBlueprintEditorUtils.
        if (bWasBoundAsVariable)
        {
            FBlueprintEditorUtils::RenameMemberVariable(WBP, OldFName, NewFName);
        }

        // Reconcile + recompile. The reconcile pass clears stale variable GUIDs
        // for the legacy name; MarkAsStructurallyModified bumps the
        // BlueprintCompileVersion so the next CDO load picks up the new layout.
        MonolithUIInternal::ReconcileWidgetVariableGuids(WBP);
        FBlueprintEditorUtils::MarkBlueprintAsStructurallyModified(WBP);
        FKismetEditorUtilities::CompileBlueprint(WBP);
        WBP->GetOutermost()->MarkPackageDirty();

        TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
        Result->SetStringField(TEXT("wbp_path"), WbpPath);
        Result->SetStringField(TEXT("old_name"), OldName);
        Result->SetStringField(TEXT("new_name"), NewName);
        Result->SetStringField(TEXT("widget_class"), OldClass);
        Result->SetBoolField(TEXT("was_bound_as_variable"), bWasBoundAsVariable);
        Result->SetBoolField(TEXT("recompiled"), true);
        return FMonolithActionResult::Success(Result);
    }

    // ---- Phase 2 Item #14 — dump_blueprint_compile_log ------------------------
    //
    // Re-drives a compile through the FCompilerResultsLog-capturing overload
    // and returns the messages in the same shape blueprint_query("compile_blueprint")
    // produces. Phase 1's HandleCompileWidget does NOT cache the FCompilerResultsLog
    // on the asset — the Phase 1 fix surfaced the log only inside its own call.
    // dump_blueprint_compile_log is intended to be called AFTER a compile when
    // the orchestrator dropped or never parsed the original payload.
    //
    // Accepts both UWidgetBlueprint and plain UBlueprint paths so the action
    // works as a general-purpose "last status + messages" probe.

    FMonolithActionResult HandleDumpBlueprintCompileLog(const TSharedPtr<FJsonObject>& Params)
    {
        FString AssetPath;
        FMonolithActionResult ParamError;
        if (!MonolithUIInternal::TryGetRequiredString(Params, TEXT("asset_path"), AssetPath, ParamError))
            return ParamError;

        const FString RequestedAssetPath = AssetPath;
        const FString BlueprintObjectPath = FPackageName::ExportTextPathToObjectPath(AssetPath);

        UBlueprint* Blueprint = FMonolithAssetUtils::LoadAssetByPath<UBlueprint>(BlueprintObjectPath);
        if (!Blueprint)
        {
            return FMonolithActionResult::Error(
                FString::Printf(TEXT("Failed to load Blueprint at '%s'"), *RequestedAssetPath),
                -32602);
        }

        // Capture pre-compile status so callers can tell whether the action
        // observed a stale BS_Dirty / BS_UpToDate from a previous edit
        // (vs. forcing a fresh compile because the asset was clean).
        const TEnumAsByte<EBlueprintStatus> PreStatus = Blueprint->Status;

        FCompilerResultsLog Results;
        FKismetEditorUtilities::CompileBlueprint(
            Blueprint, EBlueprintCompileOptions::SkipGarbageCollection, &Results);

        // Status-string mapping aligned with HandleCompileWidget (line ~793).
        auto StatusToString = [](EBlueprintStatus S) -> FString
        {
            switch (S)
            {
                case BS_Unknown:              return TEXT("unknown");
                case BS_Dirty:                return TEXT("dirty");
                case BS_Error:                return TEXT("error");
                case BS_UpToDate:             return TEXT("up_to_date");
                case BS_UpToDateWithWarnings: return TEXT("up_to_date_with_warnings");
                case BS_BeingCreated:         return TEXT("being_created");
                default:                      return TEXT("other");
            }
        };

        TArray<TSharedPtr<FJsonValue>> ErrorArr;
        TArray<TSharedPtr<FJsonValue>> WarnArr;
        TArray<TSharedPtr<FJsonValue>> NoteArr;
        ErrorArr.Reserve(Results.Messages.Num());
        WarnArr.Reserve(Results.Messages.Num());
        NoteArr.Reserve(Results.Messages.Num());
        for (const TSharedRef<FTokenizedMessage>& Msg : Results.Messages)
        {
            TSharedPtr<FJsonObject> MsgObj = MakeShared<FJsonObject>();
            MsgObj->SetStringField(TEXT("message"), Msg->ToText().ToString());

            const EMessageSeverity::Type Sev = Msg->GetSeverity();
            if (Sev == EMessageSeverity::Error)
            {
                ErrorArr.Add(MakeShared<FJsonValueObject>(MsgObj));
            }
            else if (Sev == EMessageSeverity::Warning)
            {
                WarnArr.Add(MakeShared<FJsonValueObject>(MsgObj));
            }
            else
            {
                NoteArr.Add(MakeShared<FJsonValueObject>(MsgObj));
            }
        }

        TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
        Result->SetStringField(TEXT("asset_path"), AssetPath);
        Result->SetStringField(TEXT("blueprint_class"), Blueprint->GetClass()->GetName());
        Result->SetStringField(TEXT("pre_compile_status"), StatusToString(PreStatus));
        Result->SetStringField(TEXT("last_compile_status"), StatusToString(Blueprint->Status));
        Result->SetArrayField(TEXT("errors"),   ErrorArr);
        Result->SetArrayField(TEXT("warnings"), WarnArr);
        Result->SetArrayField(TEXT("notes"),    NoteArr);
        Result->SetNumberField(TEXT("error_count"),   ErrorArr.Num());
        Result->SetNumberField(TEXT("warning_count"), WarnArr.Num());
        Result->SetBoolField(TEXT("ran_fresh_compile"), true);
        return FMonolithActionResult::Success(Result);
    }
}
