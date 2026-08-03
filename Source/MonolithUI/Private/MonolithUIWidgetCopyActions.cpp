// MonolithUIWidgetCopyActions.cpp
#include "MonolithUIWidgetCopyActions.h"

#include "Blueprint/WidgetTree.h"
#include "Components/PanelSlot.h"
#include "Components/PanelWidget.h"
#include "Components/Widget.h"
#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "Engine/Engine.h"
#include "Kismet2/BlueprintEditorUtils.h"
#include "Kismet2/KismetEditorUtilities.h"
#include "Misc/PackageName.h"
#include "MonolithJsonUtils.h"
#include "MonolithParamSchema.h"
#include "MonolithParamUtils.h"
#include "MonolithUICommon.h"
#include "ScopedTransaction.h"
#include "Serialization/ArchiveReplaceObjectRef.h"
#include "UObject/Package.h"
#include "UObject/SavePackage.h"
#include "UObject/SoftObjectPath.h"
#include "UObject/UObjectIterator.h"
#include "UObject/UnrealType.h"
#include "WidgetBlueprint.h"
#include "WidgetBlueprintEditorUtils.h"
#include "MonolithAssetUtils.h"

namespace MonolithUI::WidgetCopy
{
    struct FRemapRule
    {
        FString From;
        FString To;
    };

    struct FClassRemap
    {
        FString FromSpecifier;
        FString ToSpecifier;
        UClass* FromClass = nullptr;
        UClass* ToClass = nullptr;
    };

    struct FResolvedWidgetClass
    {
        UClass* Class = nullptr;
        bool bRemapped = false;
        FString MatchedRule;
    };

    struct FSourceWidgetAttachment
    {
        UWidget* Widget = nullptr;
        UPanelWidget* Parent = nullptr;
        UPanelSlot* Slot = nullptr;
        int32 ChildIndex = INDEX_NONE;
    };

    struct FCopyOptions
    {
        FString SourceAssetPath;
        FString DestinationAssetPath;
        TArray<FString> SourceWidgetNames;
        FString DestinationWidgetName;
        FString DestinationParentName;
        FString ExistingPolicy = TEXT("fail");
        FString InsertPolicy = TEXT("source_index");
        bool bRequireRemappedClasses = false;
        bool bCompile = true;
        bool bSave = false;
        bool bDryRun = true;
        bool bConfirm = false;
        TArray<FClassRemap> ClassRemaps;
        TArray<FRemapRule> ReferenceRemaps;
    };

    struct FCopyPlan
    {
        UWidget* SourceWidget = nullptr;
        UPanelWidget* DestinationParent = nullptr;
        FName DestinationWidgetName;
        int32 SourceChildIndex = INDEX_NONE;
        TArray<UWidget*> SourceSubtree;
        TArray<FSourceWidgetAttachment> SourceAttachments;
        TArray<UWidget*> DestinationCollisions;
        TArray<FResolvedWidgetClass> ResolvedClasses;
        bool bSkip = false;
        FString SkipReason;
    };

    struct FDestinationSlotBinding
    {
        UPanelSlot* SourceSlot = nullptr;
        UPanelSlot* DestinationSlot = nullptr;
        UPanelWidget* DestinationParent = nullptr;
        UWidget* DestinationContent = nullptr;
        bool bCopySlotProperties = false;
    };

    static FString NormalizeReferencePath(FString Path)
    {
        Path.TrimStartAndEndInline();
        Path.ReplaceInline(TEXT("\\"), TEXT("/"));
        while (Path.Len() > 1 && Path.EndsWith(TEXT("/")))
        {
            Path.LeftChopInline(1);
        }
        return Path;
    }

    static FString GetPackagePathPart(const FString& ObjectOrPackagePath)
    {
        int32 DotIndex = INDEX_NONE;
        if (ObjectOrPackagePath.FindChar(TEXT('.'), DotIndex) && DotIndex > 0)
        {
            return ObjectOrPackagePath.Left(DotIndex);
        }
        return ObjectOrPackagePath;
    }

    static bool IsValidPackageRoot(const FString& Path)
    {
        return Path.StartsWith(TEXT("/")) && Path.Len() > 1 && !Path.Contains(TEXT("//"));
    }

    static bool PathMatchesPrefixBoundary(const FString& Path, const FString& Prefix)
    {
        if (Prefix.IsEmpty())
        {
            return false;
        }
        if (Path.Equals(Prefix, ESearchCase::CaseSensitive))
        {
            return true;
        }
        if (Path.Len() <= Prefix.Len() || !Path.StartsWith(Prefix, ESearchCase::CaseSensitive))
        {
            return false;
        }
        const TCHAR Boundary = Path[Prefix.Len()];
        return Boundary == TCHAR('/') || Boundary == TCHAR('.');
    }

    static bool TryRemapReferencePath(
        const FString& InPath,
        const TArray<FRemapRule>& Rules,
        FString& OutPath,
        FString& OutMatchedRule)
    {
        OutPath.Reset();
        OutMatchedRule.Reset();

        const FString NormalizedPath = NormalizeReferencePath(InPath);
        if (NormalizedPath.IsEmpty())
        {
            return false;
        }

        const FString PackagePath = GetPackagePathPart(NormalizedPath);
        for (const FRemapRule& Rule : Rules)
        {
            if (Rule.From.IsEmpty() || Rule.To.IsEmpty())
            {
                continue;
            }

            if (NormalizedPath.Equals(Rule.From, ESearchCase::CaseSensitive))
            {
                OutPath = Rule.To;
                OutMatchedRule = Rule.From;
                return true;
            }

            if (PackagePath.Equals(Rule.From, ESearchCase::CaseSensitive))
            {
                const FString ObjectSuffix = NormalizedPath.Mid(PackagePath.Len());
                OutPath = Rule.To.Contains(TEXT(".")) ? Rule.To : Rule.To + ObjectSuffix;
                OutMatchedRule = Rule.From;
                return true;
            }

            if (PathMatchesPrefixBoundary(NormalizedPath, Rule.From))
            {
                OutPath = Rule.To + NormalizedPath.Mid(Rule.From.Len());
                OutMatchedRule = Rule.From;
                return true;
            }
        }

        return false;
    }

    static void AddStringArrayField(const TSharedPtr<FJsonObject>& Obj, const FString& FieldName, const TArray<FString>& Values)
    {
        TArray<TSharedPtr<FJsonValue>> Arr;
        Arr.Reserve(Values.Num());
        for (const FString& Value : Values)
        {
            Arr.Add(MakeShared<FJsonValueString>(Value));
        }
        Obj->SetArrayField(FieldName, Arr);
    }

    static TSharedPtr<FJsonObject> MakeRemapRuleJson(const FRemapRule& Rule)
    {
        TSharedPtr<FJsonObject> Obj = MakeShared<FJsonObject>();
        Obj->SetStringField(TEXT("from"), Rule.From);
        Obj->SetStringField(TEXT("to"), Rule.To);
        return Obj;
    }

    static TArray<TSharedPtr<FJsonValue>> MakeRemapRuleArray(const TArray<FRemapRule>& Rules)
    {
        TArray<TSharedPtr<FJsonValue>> Arr;
        Arr.Reserve(Rules.Num());
        for (const FRemapRule& Rule : Rules)
        {
            Arr.Add(MakeShared<FJsonValueObject>(MakeRemapRuleJson(Rule)));
        }
        return Arr;
    }

    static bool ParseStringObjectMap(
        const TSharedPtr<FJsonObject>& Params,
        const FString& FieldName,
        TArray<FRemapRule>& OutRules,
        FString& OutError)
    {
        const TSharedPtr<FJsonObject>* MapObject = nullptr;
        if (!Params.IsValid() || !Params->TryGetObjectField(FieldName, MapObject) || !MapObject)
        {
            return true;
        }

        for (const TPair<FString, TSharedPtr<FJsonValue>>& Pair : FMonolithJsonUtils::GetFields(*MapObject))
        {
            FString TargetPath;
            if (!Pair.Value.IsValid() || !Pair.Value->TryGetString(TargetPath))
            {
                OutError = FString::Printf(TEXT("%s['%s'] must be a string target path."), *FieldName, *Pair.Key);
                return false;
            }

            FRemapRule Rule;
            Rule.From = NormalizeReferencePath(Pair.Key);
            Rule.To = NormalizeReferencePath(TargetPath);
            if (Rule.From.IsEmpty() || Rule.To.IsEmpty())
            {
                OutError = FString::Printf(TEXT("%s cannot contain empty source or target paths."), *FieldName);
                return false;
            }
            OutRules.Add(MoveTemp(Rule));
        }

        return true;
    }

    static UClass* ResolveWidgetClassSpecifier(const FString& InSpecifier)
    {
        FString Specifier = InSpecifier.TrimStartAndEnd();
        if (Specifier.IsEmpty())
        {
            return nullptr;
        }

        if (UClass* TokenClass = MonolithUI::WidgetClassFromName(Specifier))
        {
            return TokenClass->IsChildOf(UWidget::StaticClass()) ? TokenClass : nullptr;
        }

        TArray<FString> ClassPathCandidates;
        ClassPathCandidates.Add(Specifier);
        if (Specifier.StartsWith(TEXT("/Game/")) && !Specifier.EndsWith(TEXT("_C")))
        {
            ClassPathCandidates.Add(MonolithParamUtils::NormalizeBlueprintClassPath(Specifier));
        }

        for (const FString& ClassPath : ClassPathCandidates)
        {
            if (UClass* LoadedClass = FMonolithAssetUtils::LoadAssetByPath<UClass>(ClassPath))
            {
                return LoadedClass->IsChildOf(UWidget::StaticClass()) ? LoadedClass : nullptr;
            }
            if (UClass* LoadedClass = StaticLoadClass(UWidget::StaticClass(), nullptr, *ClassPath))
            {
                return LoadedClass->IsChildOf(UWidget::StaticClass()) ? LoadedClass : nullptr;
            }
        }

        return nullptr;
    }

    static TArray<FString> GetClassLookupKeys(const UClass* Class)
    {
        TArray<FString> Keys;
        if (!Class)
        {
            return Keys;
        }

        Keys.AddUnique(Class->GetPathName());
        Keys.AddUnique(Class->GetName());

        const FName Token = MonolithUI::MakeTokenFromClassName(Class);
        if (!Token.IsNone())
        {
            Keys.AddUnique(Token.ToString());
        }

        if (UObject* GeneratedBy = Class->ClassGeneratedBy)
        {
            Keys.AddUnique(GeneratedBy->GetPathName());
        }

        return Keys;
    }

    static bool ParseClassRemaps(
        const TSharedPtr<FJsonObject>& Params,
        TArray<FClassRemap>& OutClassRemaps,
        FString& OutError)
    {
        OutClassRemaps.Reset();

        const TSharedPtr<FJsonObject>* ClassRemapObject = nullptr;
        if (!Params.IsValid() || !Params->TryGetObjectField(TEXT("class_remaps"), ClassRemapObject) || !ClassRemapObject)
        {
            return true;
        }

        for (const TPair<FString, TSharedPtr<FJsonValue>>& Pair : FMonolithJsonUtils::GetFields(*ClassRemapObject))
        {
            FString TargetClassSpec;
            if (!Pair.Value.IsValid() || !Pair.Value->TryGetString(TargetClassSpec))
            {
                OutError = FString::Printf(TEXT("class_remaps['%s'] must be a string widget class path or token."), *Pair.Key);
                return false;
            }

            FClassRemap Remap;
            Remap.FromSpecifier = Pair.Key.TrimStartAndEnd();
            Remap.ToSpecifier = TargetClassSpec.TrimStartAndEnd();
            if (Remap.FromSpecifier.IsEmpty() || Remap.ToSpecifier.IsEmpty())
            {
                OutError = TEXT("class_remaps cannot contain empty source or target class specifiers.");
                return false;
            }

            Remap.FromClass = ResolveWidgetClassSpecifier(Remap.FromSpecifier);
            if (!Remap.FromClass)
            {
                OutError = FString::Printf(TEXT("Could not resolve class_remaps source '%s' as a UWidget class."), *Remap.FromSpecifier);
                return false;
            }

            Remap.ToClass = ResolveWidgetClassSpecifier(Remap.ToSpecifier);
            if (!Remap.ToClass)
            {
                OutError = FString::Printf(TEXT("Could not resolve class_remaps target '%s' as a UWidget class."), *Remap.ToSpecifier);
                return false;
            }

            OutClassRemaps.Add(MoveTemp(Remap));
        }

        return true;
    }

    static bool ParseReferenceRemaps(
        const TSharedPtr<FJsonObject>& Params,
        TArray<FRemapRule>& OutRules,
        FString& OutError)
    {
        OutRules.Reset();

        FString SourceRoot;
        FString DestRoot;
        if (!MonolithParamUtils::GetOptionalStringParam(Params, TEXT("source_root"), SourceRoot, OutError))
        {
            return false;
        }
        if (!MonolithParamUtils::GetOptionalStringParam(Params, TEXT("dest_root"), DestRoot, OutError))
        {
            return false;
        }

        SourceRoot = NormalizeReferencePath(SourceRoot);
        DestRoot = NormalizeReferencePath(DestRoot);
        if (SourceRoot.IsEmpty() != DestRoot.IsEmpty())
        {
            OutError = TEXT("source_root and dest_root must be supplied together.");
            return false;
        }
        if (!SourceRoot.IsEmpty())
        {
            if (!IsValidPackageRoot(SourceRoot) || !IsValidPackageRoot(DestRoot))
            {
                OutError = TEXT("source_root and dest_root must be long package roots beginning with '/' and must not contain '//'.");
                return false;
            }
            OutRules.Add({ SourceRoot, DestRoot });
        }

        const TSharedPtr<FJsonObject>* RootRemaps = nullptr;
        if (Params.IsValid() && Params->TryGetObjectField(TEXT("root_remaps"), RootRemaps) && RootRemaps)
        {
            for (const TPair<FString, TSharedPtr<FJsonValue>>& Pair : FMonolithJsonUtils::GetFields(*RootRemaps))
            {
                FString TargetRoot;
                if (!Pair.Value.IsValid() || !Pair.Value->TryGetString(TargetRoot))
                {
                    OutError = FString::Printf(TEXT("root_remaps['%s'] must be a string destination root."), *Pair.Key);
                    return false;
                }

                FRemapRule Rule;
                Rule.From = NormalizeReferencePath(Pair.Key);
                Rule.To = NormalizeReferencePath(TargetRoot);
                if (!IsValidPackageRoot(Rule.From) || !IsValidPackageRoot(Rule.To))
                {
                    OutError = FString::Printf(
                        TEXT("Invalid root_remaps entry '%s' -> '%s'; roots must begin with '/' and must not contain '//'."),
                        *Rule.From,
                        *Rule.To);
                    return false;
                }
                OutRules.Add(MoveTemp(Rule));
            }
        }

        if (!ParseStringObjectMap(Params, TEXT("object_remaps"), OutRules, OutError))
        {
            return false;
        }

        OutRules.Sort([](const FRemapRule& A, const FRemapRule& B)
        {
            return A.From.Len() > B.From.Len();
        });
        return true;
    }

    static bool ParseOptions(const TSharedPtr<FJsonObject>& Params, FCopyOptions& OutOptions, FString& OutError)
    {
        if (!MonolithParamUtils::GetRequiredStringParam(Params, TEXT("source_asset_path"), OutOptions.SourceAssetPath, OutError))
        {
            return false;
        }
        if (!MonolithParamUtils::GetRequiredStringParam(Params, TEXT("destination_asset_path"), OutOptions.DestinationAssetPath, OutError))
        {
            return false;
        }

        FString SourceWidgetName;
        if (!MonolithParamUtils::GetOptionalStringParam(Params, TEXT("source_widget_name"), SourceWidgetName, OutError))
        {
            return false;
        }
        if (!MonolithParamUtils::GetOptionalStringArrayParam(Params, TEXT("source_widget_names"), OutOptions.SourceWidgetNames, OutError))
        {
            return false;
        }
        if (!SourceWidgetName.IsEmpty())
        {
            OutOptions.SourceWidgetNames.Insert(SourceWidgetName, 0);
        }
        for (FString& Name : OutOptions.SourceWidgetNames)
        {
            Name.TrimStartAndEndInline();
        }
        OutOptions.SourceWidgetNames.RemoveAll([](const FString& Name) { return Name.IsEmpty(); });
        if (OutOptions.SourceWidgetNames.IsEmpty())
        {
            OutError = TEXT("Provide source_widget_name or source_widget_names.");
            return false;
        }

        if (!MonolithParamUtils::GetOptionalStringParam(Params, TEXT("destination_widget_name"), OutOptions.DestinationWidgetName, OutError))
        {
            return false;
        }
        if (!OutOptions.DestinationWidgetName.IsEmpty() && OutOptions.SourceWidgetNames.Num() != 1)
        {
            OutError = TEXT("destination_widget_name can only be used with one source widget.");
            return false;
        }

        if (!MonolithParamUtils::GetOptionalStringParam(Params, TEXT("destination_parent_name"), OutOptions.DestinationParentName, OutError))
        {
            return false;
        }
        if (!MonolithParamUtils::GetOptionalStringParam(Params, TEXT("existing_policy"), OutOptions.ExistingPolicy, OutError, TEXT("fail")))
        {
            return false;
        }
        OutOptions.ExistingPolicy = OutOptions.ExistingPolicy.ToLower();
        if (OutOptions.ExistingPolicy != TEXT("fail") && OutOptions.ExistingPolicy != TEXT("replace") && OutOptions.ExistingPolicy != TEXT("skip"))
        {
            OutError = TEXT("existing_policy must be one of: fail, replace, skip.");
            return false;
        }

        if (!MonolithParamUtils::GetOptionalStringParam(Params, TEXT("insert_policy"), OutOptions.InsertPolicy, OutError, TEXT("source_index")))
        {
            return false;
        }
        OutOptions.InsertPolicy = OutOptions.InsertPolicy.ToLower();
        if (OutOptions.InsertPolicy != TEXT("source_index") && OutOptions.InsertPolicy != TEXT("append"))
        {
            OutError = TEXT("insert_policy must be one of: source_index, append.");
            return false;
        }

        if (!MonolithParamUtils::GetOptionalBoolParam(Params, TEXT("require_remapped_classes"), OutOptions.bRequireRemappedClasses, OutError, false))
        {
            return false;
        }
        if (!MonolithParamUtils::GetOptionalBoolParam(Params, TEXT("compile"), OutOptions.bCompile, OutError, true))
        {
            return false;
        }
        if (!MonolithParamUtils::GetOptionalBoolParam(Params, TEXT("save"), OutOptions.bSave, OutError, false))
        {
            return false;
        }
        if (!MonolithParamUtils::GetOptionalBoolParam(Params, TEXT("dry_run"), OutOptions.bDryRun, OutError, true))
        {
            return false;
        }
        if (!MonolithParamUtils::GetOptionalBoolParam(Params, TEXT("confirm"), OutOptions.bConfirm, OutError, false))
        {
            return false;
        }
        if (!OutOptions.bDryRun && !OutOptions.bConfirm)
        {
            OutError = TEXT("copy_widget_subtree_with_class_remap is mutating; pass dry_run=true to inspect the plan or confirm=true with dry_run=false to apply.");
            return false;
        }

        if (!ParseClassRemaps(Params, OutOptions.ClassRemaps, OutError))
        {
            return false;
        }
        if (!ParseReferenceRemaps(Params, OutOptions.ReferenceRemaps, OutError))
        {
            return false;
        }
        if (OutOptions.ClassRemaps.IsEmpty() && OutOptions.ReferenceRemaps.IsEmpty())
        {
            OutError = TEXT("Provide class_remaps, object_remaps, root_remaps, or source_root+dest_root so the copied subtree has an explicit remap contract.");
            return false;
        }

        return true;
    }

    static void AddReplacementObject(UObject* OldObject, UObject* NewObject, TMap<UObject*, UObject*>& ReplacementObjects)
    {
        if (OldObject && NewObject && OldObject != NewObject)
        {
            ReplacementObjects.FindOrAdd(OldObject) = NewObject;
        }
    }

    static void AddClassReplacementObjects(UClass* OldClass, UClass* NewClass, TMap<UObject*, UObject*>& ReplacementObjects)
    {
        AddReplacementObject(OldClass, NewClass, ReplacementObjects);
        AddReplacementObject(
            OldClass ? OldClass->GetDefaultObject(false) : nullptr,
            NewClass ? NewClass->GetDefaultObject(false) : nullptr,
            ReplacementObjects);
    }

    static bool ShouldMapPackageContainedObject(UObject* Object)
    {
        return Object &&
            !Object->IsA<UPackage>() &&
            !Object->HasAnyFlags(RF_Transient) &&
            Object->GetClass()->GetName() != TEXT("MetaData");
    }

    static FString RemapRelativeObjectPathAssetName(
        const FString& RelativeObjectPath,
        const FString& SourceAssetName,
        const FString& DestinationAssetName)
    {
        if (SourceAssetName == DestinationAssetName)
        {
            return RelativeObjectPath;
        }

        if (RelativeObjectPath == SourceAssetName)
        {
            return DestinationAssetName;
        }

        if (RelativeObjectPath.StartsWith(SourceAssetName + TEXT(":")) ||
            RelativeObjectPath.StartsWith(SourceAssetName + TEXT(".")) ||
            RelativeObjectPath.StartsWith(SourceAssetName + TEXT("_")))
        {
            return DestinationAssetName + RelativeObjectPath.RightChop(SourceAssetName.Len());
        }

        return RelativeObjectPath;
    }

    static void AddPackageContainedReplacementObjects(
        const FString& SourcePackageName,
        const FString& DestinationPackageName,
        UPackage* SourcePackage,
        UPackage* DestinationPackage,
        TMap<UObject*, UObject*>& ReplacementObjects)
    {
        if (!SourcePackage || !DestinationPackage)
        {
            return;
        }

        SourcePackage->FullyLoad();
        DestinationPackage->FullyLoad();

        const FString SourceAssetName = FPackageName::GetLongPackageAssetName(SourcePackageName);
        const FString DestinationAssetName = FPackageName::GetLongPackageAssetName(DestinationPackageName);

        TMap<FString, UObject*> DestinationObjectsByRelativePath;
        for (TObjectIterator<UObject> ObjectIt; ObjectIt; ++ObjectIt)
        {
            UObject* Object = *ObjectIt;
            if (ShouldMapPackageContainedObject(Object) && Object->GetOutermost() == DestinationPackage)
            {
                DestinationObjectsByRelativePath.Add(Object->GetPathName(DestinationPackage), Object);
            }
        }

        for (TObjectIterator<UObject> ObjectIt; ObjectIt; ++ObjectIt)
        {
            UObject* SourceObject = *ObjectIt;
            if (!ShouldMapPackageContainedObject(SourceObject) || SourceObject->GetOutermost() != SourcePackage)
            {
                continue;
            }

            const FString SourceRelativePath = SourceObject->GetPathName(SourcePackage);
            const FString DestinationRelativePath = RemapRelativeObjectPathAssetName(SourceRelativePath, SourceAssetName, DestinationAssetName);
            UObject* const* DestinationObject = DestinationObjectsByRelativePath.Find(DestinationRelativePath);
            if (DestinationObject)
            {
                AddReplacementObject(SourceObject, *DestinationObject, ReplacementObjects);
            }
        }
    }

    static void BuildReplacementObjects(
        UWidgetBlueprint* SourceBlueprint,
        UWidgetBlueprint* DestinationBlueprint,
        const FCopyOptions& Options,
        TMap<UObject*, UObject*>& OutReplacementObjects)
    {
        AddReplacementObject(SourceBlueprint, DestinationBlueprint, OutReplacementObjects);
        AddReplacementObject(SourceBlueprint ? SourceBlueprint->WidgetTree : nullptr, DestinationBlueprint ? DestinationBlueprint->WidgetTree : nullptr, OutReplacementObjects);
        AddClassReplacementObjects(
            SourceBlueprint ? SourceBlueprint->GeneratedClass : nullptr,
            DestinationBlueprint ? DestinationBlueprint->GeneratedClass : nullptr,
            OutReplacementObjects);
        AddClassReplacementObjects(
            SourceBlueprint ? SourceBlueprint->SkeletonGeneratedClass : nullptr,
            DestinationBlueprint ? DestinationBlueprint->SkeletonGeneratedClass : nullptr,
            OutReplacementObjects);

        if (SourceBlueprint && DestinationBlueprint)
        {
            AddPackageContainedReplacementObjects(
                SourceBlueprint->GetOutermost()->GetName(),
                DestinationBlueprint->GetOutermost()->GetName(),
                SourceBlueprint->GetOutermost(),
                DestinationBlueprint->GetOutermost(),
                OutReplacementObjects);
        }

        for (const FClassRemap& Remap : Options.ClassRemaps)
        {
            AddClassReplacementObjects(Remap.FromClass, Remap.ToClass, OutReplacementObjects);
        }

        for (const FRemapRule& Rule : Options.ReferenceRemaps)
        {
            UObject* OldObject = FSoftObjectPath(Rule.From).TryLoad();
            UObject* NewObject = FSoftObjectPath(Rule.To).TryLoad();
            AddReplacementObject(OldObject, NewObject, OutReplacementObjects);
        }
    }

    static FResolvedWidgetClass ResolveDestinationClassForWidget(const UWidget* SourceWidget, const FCopyOptions& Options)
    {
        FResolvedWidgetClass Resolved;
        Resolved.Class = SourceWidget ? SourceWidget->GetClass() : nullptr;
        if (!SourceWidget || !Resolved.Class)
        {
            return Resolved;
        }

        const TArray<FString> Keys = GetClassLookupKeys(Resolved.Class);
        for (const FClassRemap& Remap : Options.ClassRemaps)
        {
            if (Remap.FromClass == Resolved.Class)
            {
                Resolved.Class = Remap.ToClass;
                Resolved.bRemapped = true;
                Resolved.MatchedRule = Remap.FromSpecifier;
                return Resolved;
            }

            for (const FString& Key : Keys)
            {
                if (Key.Equals(Remap.FromSpecifier, ESearchCase::CaseSensitive))
                {
                    Resolved.Class = Remap.ToClass;
                    Resolved.bRemapped = true;
                    Resolved.MatchedRule = Remap.FromSpecifier;
                    return Resolved;
                }
            }
        }

        return Resolved;
    }

    static void CollectWidgetSubtree(UWidget* RootWidget, TArray<UWidget*>& OutWidgets)
    {
        OutWidgets.Reset();
        if (!RootWidget)
        {
            return;
        }

        OutWidgets.Add(RootWidget);
        TArray<UWidget*> Descendants;
        UWidgetTree::GetChildWidgets(RootWidget, Descendants);
        OutWidgets.Append(Descendants);
    }

    static UPanelWidget* ResolveDestinationParent(
        UWidgetBlueprint* SourceBlueprint,
        UWidgetBlueprint* DestinationBlueprint,
        UWidget* SourceWidget,
        const FCopyOptions& Options,
        int32& OutSourceChildIndex,
        FString& OutError)
    {
        OutSourceChildIndex = INDEX_NONE;
        UPanelWidget* SourceParent = SourceWidget
            ? UWidgetTree::FindWidgetParent(SourceWidget, OutSourceChildIndex)
            : nullptr;

        if (!Options.DestinationParentName.IsEmpty())
        {
            UWidget* ExplicitParent = DestinationBlueprint && DestinationBlueprint->WidgetTree
                ? DestinationBlueprint->WidgetTree->FindWidget(FName(*Options.DestinationParentName))
                : nullptr;
            UPanelWidget* ParentPanel = Cast<UPanelWidget>(ExplicitParent);
            if (!ParentPanel)
            {
                OutError = FString::Printf(TEXT("destination_parent_name '%s' was not found or is not a panel widget."), *Options.DestinationParentName);
                return nullptr;
            }
            return ParentPanel;
        }

        if (!SourceParent || SourceWidget == SourceBlueprint->WidgetTree->RootWidget || SourceParent == SourceBlueprint->WidgetTree->RootWidget)
        {
            UPanelWidget* RootPanel = DestinationBlueprint && DestinationBlueprint->WidgetTree
                ? Cast<UPanelWidget>(DestinationBlueprint->WidgetTree->RootWidget)
                : nullptr;
            if (!RootPanel)
            {
                OutError = TEXT("Destination Widget Blueprint root is missing or is not a panel widget; provide destination_parent_name.");
                return nullptr;
            }
            return RootPanel;
        }

        UWidget* ParentWidget = DestinationBlueprint->WidgetTree->FindWidget(SourceParent->GetFName());
        UPanelWidget* ParentPanel = Cast<UPanelWidget>(ParentWidget);
        if (!ParentPanel)
        {
            OutError = FString::Printf(
                TEXT("Destination Widget Blueprint is missing source-equivalent parent '%s'; provide destination_parent_name."),
                *SourceParent->GetName());
            return nullptr;
        }
        return ParentPanel;
    }

    static bool ValidateSourceSubtreeClasses(
        const FCopyOptions& Options,
        const TArray<UWidget*>& SourceSubtree,
        TArray<FResolvedWidgetClass>& OutResolvedClasses,
        FString& OutError)
    {
        OutResolvedClasses.Reset();
        OutResolvedClasses.Reserve(SourceSubtree.Num());

        for (UWidget* SourceWidget : SourceSubtree)
        {
            FResolvedWidgetClass Resolved = ResolveDestinationClassForWidget(SourceWidget, Options);
            if (!Resolved.Class)
            {
                OutError = FString::Printf(TEXT("Could not resolve destination class for source widget '%s'."), SourceWidget ? *SourceWidget->GetName() : TEXT("<null>"));
                return false;
            }

            if (Options.bRequireRemappedClasses && !Resolved.bRemapped)
            {
                OutError = FString::Printf(
                    TEXT("Source widget '%s' uses class '%s' but require_remapped_classes=true and no class_remaps entry matched it."),
                    *SourceWidget->GetName(),
                    *SourceWidget->GetClass()->GetPathName());
                return false;
            }

            if (UPanelWidget* SourcePanel = Cast<UPanelWidget>(SourceWidget))
            {
                if (SourcePanel->GetChildrenCount() > 0 && !Resolved.Class->IsChildOf(UPanelWidget::StaticClass()))
                {
                    OutError = FString::Printf(
                        TEXT("Source widget '%s' has children, but remapped destination class '%s' is not a UPanelWidget."),
                        *SourceWidget->GetName(),
                        *Resolved.Class->GetPathName());
                    return false;
                }
            }

            OutResolvedClasses.Add(Resolved);
        }

        return true;
    }

    static bool BuildCopyPlans(
        UWidgetBlueprint* SourceBlueprint,
        UWidgetBlueprint* DestinationBlueprint,
        const FCopyOptions& Options,
        TArray<FCopyPlan>& OutPlans,
        FString& OutError)
    {
        OutPlans.Reset();
        TSet<FString> PlannedDestinationNames;

        for (int32 RootIndex = 0; RootIndex < Options.SourceWidgetNames.Num(); ++RootIndex)
        {
            const FString& SourceWidgetName = Options.SourceWidgetNames[RootIndex];
            UWidget* SourceWidget = SourceBlueprint->WidgetTree->FindWidget(FName(*SourceWidgetName));
            if (!SourceWidget)
            {
                OutError = FString::Printf(TEXT("source_widget_name '%s' was not found in source_asset_path '%s'."), *SourceWidgetName, *Options.SourceAssetPath);
                return false;
            }

            FCopyPlan Plan;
            Plan.SourceWidget = SourceWidget;
            Plan.DestinationWidgetName = !Options.DestinationWidgetName.IsEmpty()
                ? FName(*Options.DestinationWidgetName)
                : SourceWidget->GetFName();
            Plan.DestinationParent = ResolveDestinationParent(
                SourceBlueprint,
                DestinationBlueprint,
                SourceWidget,
                Options,
                Plan.SourceChildIndex,
                OutError);
            if (!Plan.DestinationParent)
            {
                return false;
            }

            CollectWidgetSubtree(SourceWidget, Plan.SourceSubtree);
            Plan.SourceAttachments.Reserve(Plan.SourceSubtree.Num());
            for (UWidget* SubtreeWidget : Plan.SourceSubtree)
            {
                FSourceWidgetAttachment& Attachment = Plan.SourceAttachments.AddDefaulted_GetRef();
                Attachment.Widget = SubtreeWidget;
                Attachment.Parent = SubtreeWidget
                    ? UWidgetTree::FindWidgetParent(SubtreeWidget, Attachment.ChildIndex)
                    : nullptr;
                Attachment.Slot = SubtreeWidget ? SubtreeWidget->Slot : nullptr;
            }
            if (!ValidateSourceSubtreeClasses(Options, Plan.SourceSubtree, Plan.ResolvedClasses, OutError))
            {
                return false;
            }

            for (int32 SubtreeIndex = 0; SubtreeIndex < Plan.SourceSubtree.Num(); ++SubtreeIndex)
            {
                UWidget* SubtreeWidget = Plan.SourceSubtree[SubtreeIndex];
                const FString DestinationName = SubtreeIndex == 0
                    ? Plan.DestinationWidgetName.ToString()
                    : SubtreeWidget->GetName();
                if (PlannedDestinationNames.Contains(DestinationName))
                {
                    OutError = FString::Printf(TEXT("Multiple copied widgets would use destination widget name '%s'."), *DestinationName);
                    return false;
                }
                PlannedDestinationNames.Add(DestinationName);

                if (UWidget* Existing = DestinationBlueprint->WidgetTree->FindWidget(FName(*DestinationName)))
                {
                    if (Existing == DestinationBlueprint->WidgetTree->RootWidget || Existing == Plan.DestinationParent)
                    {
                        OutError = FString::Printf(TEXT("Cannot replace destination widget '%s' because it is the root or selected destination parent."), *DestinationName);
                        return false;
                    }
                    Plan.DestinationCollisions.AddUnique(Existing);
                }
            }

            if (!Plan.DestinationCollisions.IsEmpty())
            {
                if (Options.ExistingPolicy == TEXT("fail"))
                {
                    TArray<FString> CollisionNames;
                    for (UWidget* Existing : Plan.DestinationCollisions)
                    {
                        CollisionNames.Add(Existing ? Existing->GetName() : TEXT("<null>"));
                    }
                    OutError = FString::Printf(TEXT("Destination already contains copied widget name(s): %s. Use existing_policy=replace or skip."), *FString::Join(CollisionNames, TEXT(", ")));
                    return false;
                }
                if (Options.ExistingPolicy == TEXT("skip"))
                {
                    Plan.bSkip = true;
                    Plan.SkipReason = TEXT("destination_widget_exists");
                }
            }

            OutPlans.Add(MoveTemp(Plan));
        }

        return true;
    }

    static bool IsPanelSlotTemplateCompatible(const UPanelWidget* SourceParent, const UPanelWidget* DestinationParent)
    {
        return SourceParent && DestinationParent && SourceParent->GetClass() == DestinationParent->GetClass();
    }

    static bool DuplicateStateIntoExistingObject(
        UObject* SourceObject,
        UObject* DestinationObject,
        const TMap<UObject*, UObject*>& ReplacementObjects,
        FString& OutError)
    {
        if (!SourceObject || !DestinationObject)
        {
            OutError = TEXT("Cannot copy state between null objects.");
            return false;
        }

        if (SourceObject->GetClass() != DestinationObject->GetClass())
        {
            // StaticDuplicateObjectEx requires the destination class to be at least as large as
            // the source class. Class remaps are explicitly allowed to target a smaller unrelated
            // class (for example RichTextBlock -> TextBlock), so use Unreal's name-based unrelated
            // object copier for that contract instead of relying on class memory layout.
            TMap<UObject*, UObject*> UnrelatedObjectReplacements = ReplacementObjects;
            UEngine::FCopyPropertiesForUnrelatedObjectsParams CopyParams;
            CopyParams.bDoDelta = false;
            CopyParams.bReplaceObjectClassReferences = false;
            CopyParams.bCopyDeprecatedProperties = false;
            CopyParams.bPreserveRootComponent = true;
            CopyParams.bPerformDuplication = false;
            CopyParams.bOnlyHandleDirectSubObjects = false;
            CopyParams.bSkipCompilerGeneratedDefaults = false;
            CopyParams.bNotifyObjectReplacement = false;
            CopyParams.bClearReferences = true;
            CopyParams.bReplaceInternalReferenceUponRead = true;
            CopyParams.OptionalReplacementMappings = &UnrelatedObjectReplacements;
            UEngine::CopyPropertiesForUnrelatedObjects(SourceObject, DestinationObject, CopyParams);

            DestinationObject->SetFlags(RF_Transactional);
            DestinationObject->Modify();
            return true;
        }

        FObjectDuplicationParameters DuplicationParameters(SourceObject, DestinationObject->GetOuter());
        DuplicationParameters.DestName = DestinationObject->GetFName();
        DuplicationParameters.DestClass = DestinationObject->GetClass();
        DuplicationParameters.FlagMask = RF_AllFlags;
        DuplicationParameters.ApplyFlags = RF_Transactional;
        DuplicationParameters.bAssignExternalPackages = false;
        DuplicationParameters.DuplicationSeed = ReplacementObjects;

        if (StaticDuplicateObjectEx(DuplicationParameters) != DestinationObject)
        {
            OutError = FString::Printf(
                TEXT("Failed to copy '%s' into the pre-created destination object '%s'."),
                *SourceObject->GetPathName(),
                *DestinationObject->GetPathName());
            return false;
        }

        DestinationObject->SetFlags(RF_Transactional);
        DestinationObject->Modify();
        return true;
    }

    static bool FixupSoftObjectPathValue(
        FSoftObjectPath& Path,
        const TArray<FRemapRule>& Remaps,
        int32& OutFixupCount)
    {
        if (Path.IsNull())
        {
            return false;
        }

        FString NewPathString;
        FString MatchedRule;
        if (!TryRemapReferencePath(Path.ToString(), Remaps, NewPathString, MatchedRule))
        {
            return false;
        }

        if (NewPathString == Path.ToString())
        {
            return false;
        }

        Path = FSoftObjectPath(NewPathString);
        ++OutFixupCount;
        return true;
    }

    static bool FixupSoftReferencesInProperty(
        FProperty* Property,
        void* ValuePtr,
        const TArray<FRemapRule>& Remaps,
        int32& OutFixupCount);

    static bool FixupSoftReferencesInStruct(
        UStruct* Struct,
        void* StructValuePtr,
        const TArray<FRemapRule>& Remaps,
        int32& OutFixupCount)
    {
        bool bChanged = false;
        for (TFieldIterator<FProperty> It(Struct); It; ++It)
        {
            FProperty* ChildProperty = *It;
            if (!ChildProperty || ChildProperty->HasAnyPropertyFlags(CPF_Transient | CPF_Deprecated))
            {
                continue;
            }

            void* ChildValuePtr = ChildProperty->ContainerPtrToValuePtr<void>(StructValuePtr);
            bChanged |= FixupSoftReferencesInProperty(ChildProperty, ChildValuePtr, Remaps, OutFixupCount);
        }
        return bChanged;
    }

    static bool FixupSoftReferencesInProperty(
        FProperty* Property,
        void* ValuePtr,
        const TArray<FRemapRule>& Remaps,
        int32& OutFixupCount)
    {
        if (!Property || !ValuePtr)
        {
            return false;
        }

        if (FSoftObjectProperty* SoftObjectProperty = CastField<FSoftObjectProperty>(Property))
        {
            FSoftObjectPtr SoftPtr = SoftObjectProperty->GetPropertyValue(ValuePtr);
            FSoftObjectPath Path = SoftPtr.ToSoftObjectPath();
            if (!FixupSoftObjectPathValue(Path, Remaps, OutFixupCount))
            {
                return false;
            }
            SoftObjectProperty->SetPropertyValue(ValuePtr, FSoftObjectPtr(Path));
            return true;
        }

        if (FStructProperty* StructProperty = CastField<FStructProperty>(Property))
        {
            if (StructProperty->Struct == TBaseStructure<FSoftObjectPath>::Get())
            {
                FSoftObjectPath* Path = static_cast<FSoftObjectPath*>(ValuePtr);
                return Path ? FixupSoftObjectPathValue(*Path, Remaps, OutFixupCount) : false;
            }
            return FixupSoftReferencesInStruct(StructProperty->Struct, ValuePtr, Remaps, OutFixupCount);
        }

        if (FArrayProperty* ArrayProperty = CastField<FArrayProperty>(Property))
        {
            bool bChanged = false;
            FScriptArrayHelper Helper(ArrayProperty, ValuePtr);
            for (int32 Index = 0; Index < Helper.Num(); ++Index)
            {
                bChanged |= FixupSoftReferencesInProperty(ArrayProperty->Inner, Helper.GetRawPtr(Index), Remaps, OutFixupCount);
            }
            return bChanged;
        }

        if (FSetProperty* SetProperty = CastField<FSetProperty>(Property))
        {
            bool bChanged = false;
            FScriptSetHelper Helper(SetProperty, ValuePtr);
            for (int32 Index = 0; Index < Helper.GetMaxIndex(); ++Index)
            {
                if (!Helper.IsValidIndex(Index))
                {
                    continue;
                }
                bChanged |= FixupSoftReferencesInProperty(SetProperty->ElementProp, Helper.GetElementPtr(Index), Remaps, OutFixupCount);
            }
            if (bChanged)
            {
                Helper.Rehash();
            }
            return bChanged;
        }

        if (FMapProperty* MapProperty = CastField<FMapProperty>(Property))
        {
            bool bChanged = false;
            FScriptMapHelper Helper(MapProperty, ValuePtr);
            for (int32 Index = 0; Index < Helper.GetMaxIndex(); ++Index)
            {
                if (!Helper.IsValidIndex(Index))
                {
                    continue;
                }
                bChanged |= FixupSoftReferencesInProperty(MapProperty->KeyProp, Helper.GetKeyPtr(Index), Remaps, OutFixupCount);
                bChanged |= FixupSoftReferencesInProperty(MapProperty->ValueProp, Helper.GetValuePtr(Index), Remaps, OutFixupCount);
            }
            if (bChanged)
            {
                Helper.Rehash();
            }
            return bChanged;
        }

        return false;
    }

    static int32 FixupSoftReferencesInObject(UObject* Object, const TArray<FRemapRule>& Remaps)
    {
        int32 FixupCount = 0;
        if (!Object || Remaps.IsEmpty())
        {
            return FixupCount;
        }

        FixupSoftReferencesInStruct(Object->GetClass(), Object, Remaps, FixupCount);
        return FixupCount;
    }

    static TSharedPtr<FJsonValue> MakeCopiedWidgetRow(
        UWidget* SourceWidget,
        UWidget* DestinationWidget,
        const FResolvedWidgetClass& ResolvedClass)
    {
        TSharedPtr<FJsonObject> Row = MakeShared<FJsonObject>();
        Row->SetStringField(TEXT("source_widget"), SourceWidget ? SourceWidget->GetName() : FString());
        Row->SetStringField(TEXT("destination_widget"), DestinationWidget ? DestinationWidget->GetName() : FString());
        Row->SetStringField(TEXT("source_class"), SourceWidget && SourceWidget->GetClass() ? SourceWidget->GetClass()->GetPathName() : FString());
        Row->SetStringField(TEXT("destination_class"), DestinationWidget && DestinationWidget->GetClass() ? DestinationWidget->GetClass()->GetPathName() : FString());
        Row->SetBoolField(TEXT("class_remapped"), ResolvedClass.bRemapped);
        if (!ResolvedClass.MatchedRule.IsEmpty())
        {
            Row->SetStringField(TEXT("matched_class_remap"), ResolvedClass.MatchedRule);
        }
        return MakeShared<FJsonValueObject>(Row);
    }

    static bool CreateDestinationWidgetObjects(
        UWidgetBlueprint* DestinationBlueprint,
        const TArray<FCopyPlan>& Plans,
        TMap<UWidget*, UWidget*>& OutWidgetCopies,
        TMap<UObject*, UObject*>& InOutReplacementObjects,
        TArray<TSharedPtr<FJsonValue>>& OutCopiedRows,
        FString& OutError)
    {
        if (!DestinationBlueprint || !DestinationBlueprint->WidgetTree)
        {
            OutError = TEXT("Destination Widget Blueprint has no WidgetTree.");
            return false;
        }

        for (const FCopyPlan& Plan : Plans)
        {
            if (Plan.bSkip)
            {
                continue;
            }

            for (int32 WidgetIndex = 0; WidgetIndex < Plan.SourceSubtree.Num(); ++WidgetIndex)
            {
                UWidget* SourceWidget = Plan.SourceSubtree[WidgetIndex];
                const FResolvedWidgetClass& ResolvedClass = Plan.ResolvedClasses[WidgetIndex];
                const FName DestinationName = WidgetIndex == 0
                    ? Plan.DestinationWidgetName
                    : SourceWidget->GetFName();

                if (OutWidgetCopies.Contains(SourceWidget))
                {
                    OutError = FString::Printf(TEXT("Source widget '%s' appears in more than one copy plan."), *SourceWidget->GetPathName());
                    return false;
                }

                UWidget* DestinationWidget = DestinationBlueprint->WidgetTree->ConstructWidget<UWidget>(
                    ResolvedClass.Class,
                    DestinationName);
                if (!DestinationWidget)
                {
                    OutError = FString::Printf(
                        TEXT("Failed to construct destination widget '%s' as class '%s'."),
                        *DestinationName.ToString(),
                        ResolvedClass.Class ? *ResolvedClass.Class->GetPathName() : TEXT("<null>"));
                    return false;
                }

                DestinationWidget->SetFlags(RF_Transactional);
                DestinationWidget->Modify();
                MonolithUI::RegisterCreatedWidget(DestinationBlueprint, DestinationWidget);
                OutWidgetCopies.Add(SourceWidget, DestinationWidget);
                AddReplacementObject(SourceWidget, DestinationWidget, InOutReplacementObjects);
                OutCopiedRows.Add(MakeCopiedWidgetRow(SourceWidget, DestinationWidget, ResolvedClass));
            }
        }

        return true;
    }

    static bool AttachDestinationWidget(
        UPanelWidget* DestinationParent,
        UWidget* DestinationContent,
        UPanelWidget* SourceParent,
        UWidget* SourceContent,
        UPanelSlot* SourceSlot,
        int32 InsertIndex,
        TMap<UObject*, UObject*>& InOutReplacementObjects,
        TArray<FDestinationSlotBinding>& OutSlotBindings,
        FString& OutError)
    {
        if (!DestinationParent || !DestinationContent || !SourceContent)
        {
            OutError = TEXT("Invalid widget attachment context.");
            return false;
        }

        DestinationParent->SetFlags(RF_Transactional);
        DestinationParent->Modify();

        UPanelSlot* DestinationSlot = InsertIndex == INDEX_NONE
            ? DestinationParent->AddChild(DestinationContent)
            : DestinationParent->InsertChildAt(InsertIndex, DestinationContent);
        if (!DestinationSlot)
        {
            OutError = FString::Printf(
                TEXT("Failed to attach copied widget '%s' to destination parent '%s'."),
                *DestinationContent->GetName(),
                *DestinationParent->GetName());
            return false;
        }

        FDestinationSlotBinding& Binding = OutSlotBindings.AddDefaulted_GetRef();
        Binding.SourceSlot = SourceSlot;
        Binding.DestinationSlot = DestinationSlot;
        Binding.DestinationParent = DestinationParent;
        Binding.DestinationContent = DestinationContent;
        Binding.bCopySlotProperties =
            Binding.SourceSlot &&
            IsPanelSlotTemplateCompatible(SourceParent, DestinationParent) &&
            Binding.SourceSlot->GetClass() == DestinationSlot->GetClass();

        AddReplacementObject(Binding.SourceSlot, DestinationSlot, InOutReplacementObjects);
        return true;
    }

    static bool AttachDestinationWidgetTrees(
        const TArray<FCopyPlan>& Plans,
        const FCopyOptions& Options,
        const TMap<UWidget*, UWidget*>& WidgetCopies,
        TMap<UObject*, UObject*>& InOutReplacementObjects,
        TArray<FDestinationSlotBinding>& OutSlotBindings,
        FString& OutError)
    {
        for (const FCopyPlan& Plan : Plans)
        {
            if (Plan.bSkip)
            {
                continue;
            }

            if (Plan.SourceAttachments.Num() != Plan.SourceSubtree.Num() || Plan.SourceAttachments.IsEmpty())
            {
                OutError = TEXT("Source widget attachment snapshots do not match the planned subtree.");
                return false;
            }

            // Use the pre-mutation attachment snapshot rather than the live source tree. With
            // source_asset_path == destination_asset_path and existing_policy=replace, collision
            // deletion intentionally detaches the source widgets before the destination hierarchy
            // is assembled; consulting the live tree at that point would lose slot properties.
            for (int32 AttachmentIndex = 1; AttachmentIndex < Plan.SourceAttachments.Num(); ++AttachmentIndex)
            {
                const FSourceWidgetAttachment& Attachment = Plan.SourceAttachments[AttachmentIndex];
                UWidget* DestinationChild = WidgetCopies.FindRef(Attachment.Widget);
                UPanelWidget* DestinationPanel = Cast<UPanelWidget>(WidgetCopies.FindRef(Attachment.Parent));
                if (!Attachment.Widget || !Attachment.Parent || !DestinationChild || !DestinationPanel)
                {
                    OutError = FString::Printf(
                        TEXT("Could not resolve the captured parent attachment for copied widget '%s'."),
                        Attachment.Widget ? *Attachment.Widget->GetName() : TEXT("<null>"));
                    return false;
                }

                if (!AttachDestinationWidget(
                        DestinationPanel,
                        DestinationChild,
                        Attachment.Parent,
                        Attachment.Widget,
                        Attachment.Slot,
                        Attachment.ChildIndex,
                        InOutReplacementObjects,
                        OutSlotBindings,
                        OutError))
                {
                    return false;
                }
            }

            const FSourceWidgetAttachment& RootAttachment = Plan.SourceAttachments[0];
            UWidget* DestinationRoot = WidgetCopies.FindRef(Plan.SourceWidget);
            const int32 InsertIndex = Options.InsertPolicy == TEXT("append") || Plan.SourceChildIndex == INDEX_NONE
                ? INDEX_NONE
                : FMath::Min(Plan.SourceChildIndex, Plan.DestinationParent->GetChildrenCount());
            if (!AttachDestinationWidget(
                    Plan.DestinationParent,
                    DestinationRoot,
                    RootAttachment.Parent,
                    Plan.SourceWidget,
                    RootAttachment.Slot,
                    InsertIndex,
                    InOutReplacementObjects,
                    OutSlotBindings,
                    OutError))
            {
                return false;
            }
        }

        return true;
    }

    static bool ApplyDestinationWidgetState(
        UWidgetBlueprint* DestinationBlueprint,
        const FCopyOptions& Options,
        const TMap<UWidget*, UWidget*>& WidgetCopies,
        const TMap<UObject*, UObject*>& ReplacementObjects,
        const TArray<FDestinationSlotBinding>& SlotBindings,
        int32& OutSoftReferenceFixups,
        FString& OutError)
    {
        for (const TPair<UWidget*, UWidget*>& WidgetCopy : WidgetCopies)
        {
            if (!DuplicateStateIntoExistingObject(WidgetCopy.Key, WidgetCopy.Value, ReplacementObjects, OutError))
            {
                return false;
            }

            if (!ReplacementObjects.IsEmpty())
            {
                FArchiveReplaceObjectRef<UObject> ReplaceArchive(
                    WidgetCopy.Value,
                    const_cast<TMap<UObject*, UObject*>&>(ReplacementObjects),
                    EArchiveReplaceObjectFlags::IgnoreOuterRef | EArchiveReplaceObjectFlags::IncludeClassGeneratedByRef);
            }
            OutSoftReferenceFixups += FixupSoftReferencesInObject(WidgetCopy.Value, Options.ReferenceRemaps);
        }

        for (const FDestinationSlotBinding& Binding : SlotBindings)
        {
            if (Binding.bCopySlotProperties &&
                !DuplicateStateIntoExistingObject(Binding.SourceSlot, Binding.DestinationSlot, ReplacementObjects, OutError))
            {
                return false;
            }

            if (!Binding.DestinationSlot || !Binding.DestinationParent || !Binding.DestinationContent)
            {
                OutError = TEXT("A copied widget slot binding became invalid while applying state.");
                return false;
            }

            Binding.DestinationSlot->Parent = Binding.DestinationParent;
            Binding.DestinationSlot->Content = Binding.DestinationContent;
            Binding.DestinationContent->Slot = Binding.DestinationSlot;
            if (Binding.DestinationParent->GetChildIndex(Binding.DestinationContent) == INDEX_NONE)
            {
                OutError = FString::Printf(
                    TEXT("Copied widget '%s' is not owned by its expected destination parent '%s' after state transfer."),
                    *Binding.DestinationContent->GetName(),
                    *Binding.DestinationParent->GetName());
                return false;
            }
        }

        for (const TPair<UWidget*, UWidget*>& WidgetCopy : WidgetCopies)
        {
            UWidget* DestinationWidget = WidgetCopy.Value;
            if (!DestinationWidget || DestinationWidget->GetOuter() != DestinationBlueprint->WidgetTree)
            {
                OutError = FString::Printf(
                    TEXT("Copied widget '%s' is not owned by the destination WidgetTree."),
                    DestinationWidget ? *DestinationWidget->GetName() : TEXT("<null>"));
                return false;
            }
        }

        return true;
    }

    static bool DeleteDestinationCollisions(UWidgetBlueprint* DestinationBlueprint, const TArray<FCopyPlan>& Plans, FString& OutError)
    {
        TSet<UWidget*> WidgetsToDelete;
        for (const FCopyPlan& Plan : Plans)
        {
            if (Plan.bSkip)
            {
                continue;
            }
            for (UWidget* Existing : Plan.DestinationCollisions)
            {
                if (Existing)
                {
                    WidgetsToDelete.Add(Existing);
                }
            }
        }

        if (WidgetsToDelete.IsEmpty())
        {
            return true;
        }

        FWidgetBlueprintEditorUtils::DeleteWidgets(
            DestinationBlueprint,
            WidgetsToDelete,
            FWidgetBlueprintEditorUtils::EDeleteWidgetWarningType::DeleteSilently);

        for (UWidget* Existing : WidgetsToDelete)
        {
            if (Existing && Existing->GetOuter() != GetTransientPackage())
            {
                Existing->Rename(nullptr, GetTransientPackage(), REN_DontCreateRedirectors | REN_DoNotDirty | REN_AllowPackageLinkerMismatch);
            }
        }

        return true;
    }

    static TSharedPtr<FJsonObject> MakePlanRow(const FCopyPlan& Plan)
    {
        TSharedPtr<FJsonObject> Row = MakeShared<FJsonObject>();
        Row->SetStringField(TEXT("source_widget"), Plan.SourceWidget ? Plan.SourceWidget->GetName() : FString());
        Row->SetStringField(TEXT("destination_widget"), Plan.DestinationWidgetName.ToString());
        Row->SetStringField(TEXT("destination_parent"), Plan.DestinationParent ? Plan.DestinationParent->GetName() : FString());
        Row->SetNumberField(TEXT("source_child_index"), Plan.SourceChildIndex);
        Row->SetNumberField(TEXT("source_subtree_widget_count"), Plan.SourceSubtree.Num());
        Row->SetNumberField(TEXT("collision_count"), Plan.DestinationCollisions.Num());
        Row->SetStringField(TEXT("status"), Plan.bSkip ? TEXT("skipped") : TEXT("planned"));
        if (!Plan.SkipReason.IsEmpty())
        {
            Row->SetStringField(TEXT("skip_reason"), Plan.SkipReason);
        }

        TArray<TSharedPtr<FJsonValue>> ClassRows;
        ClassRows.Reserve(FMath::Min(Plan.SourceSubtree.Num(), Plan.ResolvedClasses.Num()));
        for (int32 Index = 0; Index < Plan.SourceSubtree.Num() && Index < Plan.ResolvedClasses.Num(); ++Index)
        {
            UWidget* SourceWidget = Plan.SourceSubtree[Index];
            const FResolvedWidgetClass& Resolved = Plan.ResolvedClasses[Index];
            TSharedPtr<FJsonObject> ClassRow = MakeShared<FJsonObject>();
            ClassRow->SetStringField(TEXT("widget"), SourceWidget ? SourceWidget->GetName() : FString());
            ClassRow->SetStringField(TEXT("source_class"), SourceWidget && SourceWidget->GetClass() ? SourceWidget->GetClass()->GetPathName() : FString());
            ClassRow->SetStringField(TEXT("destination_class"), Resolved.Class ? Resolved.Class->GetPathName() : FString());
            ClassRow->SetBoolField(TEXT("class_remapped"), Resolved.bRemapped);
            if (!Resolved.MatchedRule.IsEmpty())
            {
                ClassRow->SetStringField(TEXT("matched_rule"), Resolved.MatchedRule);
            }
            ClassRows.Add(MakeShared<FJsonValueObject>(ClassRow));
        }
        Row->SetArrayField(TEXT("class_plan"), ClassRows);
        return Row;
    }

    static bool SaveDestinationPackageIfRequested(UWidgetBlueprint* DestinationBlueprint, bool bSave, FString& OutSavedFilename, FString& OutError)
    {
        OutSavedFilename.Reset();
        OutError.Reset();
        if (!bSave)
        {
            return true;
        }

        if (!DestinationBlueprint || !DestinationBlueprint->GetOutermost())
        {
            OutError = TEXT("Destination Widget Blueprint package is invalid.");
            return false;
        }

        UPackage* Package = DestinationBlueprint->GetOutermost();
        OutSavedFilename = FPackageName::LongPackageNameToFilename(
            Package->GetName(),
            FPackageName::GetAssetPackageExtension());

        FSavePackageArgs SaveArgs;
        SaveArgs.TopLevelFlags = RF_Public | RF_Standalone;
        SaveArgs.SaveFlags = SAVE_NoError;
        if (!UPackage::SavePackage(Package, nullptr, *OutSavedFilename, SaveArgs))
        {
            OutError = FString::Printf(TEXT("UPackage::SavePackage failed for '%s'."), *OutSavedFilename);
            return false;
        }
        return true;
    }
}

void FMonolithUIWidgetCopyActions::RegisterActions(FMonolithToolRegistry& Registry)
{
    Registry.RegisterAction(
        TEXT("ui"),
        TEXT("copy_widget_subtree_with_class_remap"),
        TEXT("Copy one or more source Widget Blueprint subtrees into a destination Widget Blueprint while remapping widget classes and object/package references. Dry-run is the default; mutating calls require confirm=true."),
        FMonolithActionHandler::CreateStatic(&HandleCopyWidgetSubtreeWithClassRemap),
        FParamSchemaBuilder()
            .RequiredAssetPath(TEXT("source_asset_path"), TEXT("Source Widget Blueprint asset path"))
            .RequiredAssetPath(TEXT("destination_asset_path"), TEXT("Destination Widget Blueprint asset path"))
            .Optional(TEXT("source_widget_name"), TEXT("string"), TEXT("Single source widget name to copy"))
            .Optional(TEXT("source_widget_names"), TEXT("array"), TEXT("Array of source widget names to copy"))
            .Optional(TEXT("destination_widget_name"), TEXT("string"), TEXT("Destination name override; only valid with one source widget"))
            .Optional(TEXT("destination_parent_name"), TEXT("string"), TEXT("Destination panel widget name; defaults to source-equivalent parent or destination root"))
            .Optional(TEXT("class_remaps"), TEXT("object"), TEXT("Map source widget class path/token to destination widget class path/token"))
            .Optional(TEXT("object_remaps"), TEXT("object"), TEXT("Exact object path remaps used for hard/soft references"))
            .Optional(TEXT("root_remaps"), TEXT("object"), TEXT("Map of source package roots to destination roots, e.g. {\"/Game/Old\":\"/Game/New\"}"))
            .Optional(TEXT("source_root"), TEXT("string"), TEXT("Single source root shorthand; must be supplied with dest_root"))
            .Optional(TEXT("dest_root"), TEXT("string"), TEXT("Single destination root shorthand; must be supplied with source_root"))
            .Optional(TEXT("existing_policy"), TEXT("string"), TEXT("How to handle destination name collisions: fail, replace, or skip"), TEXT("fail")).Enum(TEXT("existing_policy"), { TEXT("fail"), TEXT("replace"), TEXT("skip") })
            .Optional(TEXT("insert_policy"), TEXT("string"), TEXT("Where to insert copied roots: source_index or append"), TEXT("source_index")).Enum(TEXT("insert_policy"), { TEXT("source_index"), TEXT("append") })
            .Optional(TEXT("require_remapped_classes"), TEXT("boolean"), TEXT("Require every copied widget class to match class_remaps"), TEXT("false"))
            .Optional(TEXT("compile"), TEXT("boolean"), TEXT("Compile the destination Widget Blueprint after applying"), TEXT("true"))
            .Optional(TEXT("save"), TEXT("boolean"), TEXT("Save the destination package after applying"), TEXT("false"))
            .Optional(TEXT("dry_run"), TEXT("boolean"), TEXT("Plan without mutating the destination Widget Blueprint"), TEXT("true"))
            .Optional(TEXT("confirm"), TEXT("boolean"), TEXT("Required true when dry_run=false"), TEXT("false"))
            .Build(),
        TEXT("PostCopyRepair")
    );

    FMonolithToolRegistry::Get().SetActionSearchMetadata(
        TEXT("ui"),
        TEXT("copy_widget_subtree_with_class_remap"),
        { TEXT("Widget Blueprint subtree copy"), TEXT("class remap"), TEXT("copied UI repair"), TEXT("post-copy UI repair"), TEXT("UMG widget tree remap") },
        { TEXT("copy_wbp_subtree"), TEXT("restore_widget_subtree"), TEXT("remap_widget_subtree") },
        { TEXT("copy HostSessionPanel from a source WBP and remap CommonUI classes into the destination WBP"), TEXT("dry-run a Widget Blueprint subtree class remap before applying it") });
}

FMonolithActionResult FMonolithUIWidgetCopyActions::HandleCopyWidgetSubtreeWithClassRemap(const TSharedPtr<FJsonObject>& Params)
{
    using namespace MonolithUI::WidgetCopy;

    FCopyOptions Options;
    FString ErrorMsg;
    if (!ParseOptions(Params, Options, ErrorMsg))
    {
        return FMonolithActionResult::Error(ErrorMsg, FMonolithJsonUtils::ErrInvalidParams);
    }

    FMonolithActionResult LoadError;
    UWidgetBlueprint* SourceBlueprint = MonolithUI::LoadWidgetBlueprint(Options.SourceAssetPath, LoadError);
    if (!SourceBlueprint)
    {
        return LoadError;
    }

    UWidgetBlueprint* DestinationBlueprint = MonolithUI::LoadWidgetBlueprint(Options.DestinationAssetPath, LoadError);
    if (!DestinationBlueprint)
    {
        return LoadError;
    }

    if (!SourceBlueprint->WidgetTree || !DestinationBlueprint->WidgetTree)
    {
        return FMonolithActionResult::Error(TEXT("Source and destination Widget Blueprints must both have WidgetTree objects."));
    }

    TArray<FCopyPlan> Plans;
    if (!BuildCopyPlans(SourceBlueprint, DestinationBlueprint, Options, Plans, ErrorMsg))
    {
        return FMonolithActionResult::Error(ErrorMsg, FMonolithJsonUtils::ErrInvalidParams);
    }

    TArray<TSharedPtr<FJsonValue>> PlanRows;
    PlanRows.Reserve(Plans.Num());
    int32 PlannedCopiedWidgets = 0;
    int32 PlannedRemappedWidgets = 0;
    int32 SkippedRootCount = 0;
    for (const FCopyPlan& Plan : Plans)
    {
        PlanRows.Add(MakeShared<FJsonValueObject>(MakePlanRow(Plan)));
        if (Plan.bSkip)
        {
            ++SkippedRootCount;
            continue;
        }

        PlannedCopiedWidgets += Plan.SourceSubtree.Num();
        for (const FResolvedWidgetClass& ResolvedClass : Plan.ResolvedClasses)
        {
            if (ResolvedClass.bRemapped)
            {
                ++PlannedRemappedWidgets;
            }
        }
    }

    TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
    Result->SetStringField(TEXT("namespace"), TEXT("ui"));
    Result->SetStringField(TEXT("action"), TEXT("copy_widget_subtree_with_class_remap"));
    Result->SetStringField(TEXT("source_asset_path"), Options.SourceAssetPath);
    Result->SetStringField(TEXT("destination_asset_path"), Options.DestinationAssetPath);
    Result->SetBoolField(TEXT("dry_run"), Options.bDryRun);
    Result->SetBoolField(TEXT("confirm"), Options.bConfirm);
    Result->SetStringField(TEXT("existing_policy"), Options.ExistingPolicy);
    Result->SetStringField(TEXT("insert_policy"), Options.InsertPolicy);
    Result->SetNumberField(TEXT("planned_root_count"), Plans.Num());
    Result->SetNumberField(TEXT("planned_copied_widget_count"), PlannedCopiedWidgets);
    Result->SetNumberField(TEXT("planned_remapped_widget_count"), PlannedRemappedWidgets);
    Result->SetNumberField(TEXT("skipped_root_count"), SkippedRootCount);
    Result->SetArrayField(TEXT("plan"), PlanRows);
    Result->SetArrayField(TEXT("reference_remaps"), MakeRemapRuleArray(Options.ReferenceRemaps));

    if (Options.bDryRun)
    {
        Result->SetBoolField(TEXT("changed"), false);
        Result->SetBoolField(TEXT("compiled"), false);
        Result->SetBoolField(TEXT("saved"), false);
        return FMonolithActionResult::Success(Result);
    }

    TMap<UObject*, UObject*> ReplacementObjects;
    BuildReplacementObjects(SourceBlueprint, DestinationBlueprint, Options, ReplacementObjects);

    TArray<TSharedPtr<FJsonValue>> CopiedRows;
    CopiedRows.Reserve(PlannedCopiedWidgets);
    TMap<UWidget*, UWidget*> WidgetCopies;
    WidgetCopies.Reserve(PlannedCopiedWidgets);
    TArray<FDestinationSlotBinding> SlotBindings;
    int32 SoftReferenceFixups = 0;
    bool bChanged = false;

    const FScopedTransaction Transaction(NSLOCTEXT("MonolithUI", "CopyWidgetSubtreeWithClassRemap", "Copy Widget Subtree With Class Remap"));
    DestinationBlueprint->SetFlags(RF_Transactional);
    DestinationBlueprint->Modify();
    DestinationBlueprint->WidgetTree->SetFlags(RF_Transactional);
    DestinationBlueprint->WidgetTree->Modify();

    if (!DeleteDestinationCollisions(DestinationBlueprint, Plans, ErrorMsg))
    {
        return FMonolithActionResult::Error(ErrorMsg);
    }

    if (!CreateDestinationWidgetObjects(
            DestinationBlueprint,
            Plans,
            WidgetCopies,
            ReplacementObjects,
            CopiedRows,
            ErrorMsg))
    {
        return FMonolithActionResult::Error(ErrorMsg);
    }

    if (!AttachDestinationWidgetTrees(
            Plans,
            Options,
            WidgetCopies,
            ReplacementObjects,
            SlotBindings,
            ErrorMsg))
    {
        return FMonolithActionResult::Error(ErrorMsg);
    }

    if (!ApplyDestinationWidgetState(
            DestinationBlueprint,
            Options,
            WidgetCopies,
            ReplacementObjects,
            SlotBindings,
            SoftReferenceFixups,
            ErrorMsg))
    {
        return FMonolithActionResult::Error(ErrorMsg);
    }

    bChanged = !WidgetCopies.IsEmpty();

    bool bCompiled = false;
    if (bChanged)
    {
        FBlueprintEditorUtils::MarkBlueprintAsStructurallyModified(DestinationBlueprint);
        MonolithUI::ReconcileWidgetVariableGuids(DestinationBlueprint);
        if (Options.bCompile)
        {
            FKismetEditorUtilities::CompileBlueprint(DestinationBlueprint);
            bCompiled = true;
        }
        DestinationBlueprint->GetOutermost()->MarkPackageDirty();
    }

    FString SavedFilename;
    if (!SaveDestinationPackageIfRequested(DestinationBlueprint, Options.bSave && bChanged, SavedFilename, ErrorMsg))
    {
        return FMonolithActionResult::Error(ErrorMsg);
    }

    Result->SetBoolField(TEXT("changed"), bChanged);
    Result->SetBoolField(TEXT("compiled"), bCompiled);
    Result->SetBoolField(TEXT("saved"), !SavedFilename.IsEmpty());
    if (!SavedFilename.IsEmpty())
    {
        Result->SetStringField(TEXT("saved_filename"), SavedFilename);
    }
    Result->SetNumberField(TEXT("copied_widget_count"), CopiedRows.Num());
    Result->SetNumberField(TEXT("soft_reference_fixup_count"), SoftReferenceFixups);
    Result->SetNumberField(TEXT("replacement_object_count"), ReplacementObjects.Num());
    Result->SetArrayField(TEXT("copied_widgets"), CopiedRows);
    return FMonolithActionResult::Success(Result);
}
