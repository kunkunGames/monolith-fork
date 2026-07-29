// MonolithUIFontRepairActions.cpp
#include "MonolithUIFontRepairActions.h"

#include "AssetRegistry/AssetRegistryModule.h"
#include "AssetRegistry/IAssetRegistry.h"
#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "Engine/Font.h"
#include "Engine/FontFace.h"
#include "Engine/World.h"
#include "Fonts/CompositeFont.h"
#include "Fonts/SlateFontInfo.h"
#include "Framework/Application/SlateApplication.h"
#include "Misc/PackageName.h"
#include "MonolithAssetUtils.h"
#include "MonolithJsonUtils.h"
#include "MonolithPackagePathValidator.h"
#include "MonolithParamSchema.h"
#include "MonolithParamUtils.h"
#include "ScopedTransaction.h"
#include "UObject/Package.h"
#include "UObject/SavePackage.h"
#include "UObject/UObjectGlobals.h"
#include "UObject/UObjectIterator.h"
#include "UObject/UnrealType.h"

namespace MonolithUI::FontRepair
{
    struct FRemapRule
    {
        FString From;
        FString To;
    };

    struct FFaceChange
    {
        FString Section;
        FString Typeface;
        FString FromPath;
        FString ToPath;
        FString MatchedRule;
        FString Status;
        FString Error;
        int32 SubFaceIndex = 0;
        UFontFace* TargetFace = nullptr;

        bool IsError() const
        {
            return !Error.IsEmpty();
        }

        bool IsRemap() const
        {
            return !FromPath.IsEmpty() && !ToPath.IsEmpty() && FromPath != ToPath;
        }
    };

    static FString NormalizeCopyRepairPath(FString Path)
    {
        Path.TrimStartAndEndInline();
        Path.ReplaceInline(TEXT("\\"), TEXT("/"));
        while (Path.Len() > 1 && Path.EndsWith(TEXT("/")))
        {
            Path.LeftChopInline(1);
        }
        return Path;
    }

    static bool IsValidCopyRepairRoot(const FString& Path)
    {
        return Path.StartsWith(TEXT("/")) && Path.Len() > 1 && !Path.Contains(TEXT("//"));
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

    static bool TryRemapPath(
        const FString& InPath,
        const TArray<FRemapRule>& Rules,
        FString& OutPath,
        FString& OutMatchedFrom)
    {
        OutPath.Reset();
        OutMatchedFrom.Reset();
        const FString NormalizedPath = NormalizeCopyRepairPath(InPath);
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
                OutMatchedFrom = Rule.From;
                return true;
            }

            if (PackagePath.Equals(Rule.From, ESearchCase::CaseSensitive))
            {
                const FString ObjectSuffix = NormalizedPath.Mid(PackagePath.Len());
                OutPath = Rule.To.Contains(TEXT(".")) ? Rule.To : Rule.To + ObjectSuffix;
                OutMatchedFrom = Rule.From;
                return true;
            }

            if (PathMatchesPrefixBoundary(NormalizedPath, Rule.From))
            {
                OutPath = Rule.To + NormalizedPath.Mid(Rule.From.Len());
                OutMatchedFrom = Rule.From;
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

    static TSharedPtr<FJsonObject> MakeFaceChangeJson(const FFaceChange& Change)
    {
        TSharedPtr<FJsonObject> Obj = MakeShared<FJsonObject>();
        Obj->SetStringField(TEXT("section"), Change.Section);
        Obj->SetStringField(TEXT("typeface"), Change.Typeface);
        Obj->SetStringField(TEXT("status"), Change.Status);
        Obj->SetNumberField(TEXT("sub_face_index"), Change.SubFaceIndex);
        if (!Change.FromPath.IsEmpty())
        {
            Obj->SetStringField(TEXT("from"), Change.FromPath);
        }
        if (!Change.ToPath.IsEmpty())
        {
            Obj->SetStringField(TEXT("to"), Change.ToPath);
        }
        if (!Change.MatchedRule.IsEmpty())
        {
            Obj->SetStringField(TEXT("matched_rule"), Change.MatchedRule);
        }
        if (!Change.Error.IsEmpty())
        {
            Obj->SetStringField(TEXT("error"), Change.Error);
        }
        return Obj;
    }

    static bool PackageOrAssetExists(IAssetRegistry& AssetRegistry, const FString& PackageName, const FString& AssetName)
    {
        if (FindPackage(nullptr, *PackageName))
        {
            return true;
        }

        FString ExistingPackageFilename;
        if (FPackageName::DoesPackageExist(PackageName, &ExistingPackageFilename))
        {
            return true;
        }

        const FString ObjectPath = FString::Printf(TEXT("%s.%s"), *PackageName, *AssetName);
        return AssetRegistry.GetAssetByObjectPath(FSoftObjectPath(ObjectPath)).IsValid();
    }

    static bool NormalizeAssetPackagePath(
        const FString& InPath,
        FString& OutPackagePath,
        FString& OutAssetName,
        FString& OutError)
    {
        OutPackagePath.Reset();
        OutAssetName.Reset();
        OutError.Reset();

        FString Resolved = FMonolithAssetUtils::ResolveAssetPath(InPath);
        Resolved = NormalizeCopyRepairPath(Resolved);
        if (Resolved.IsEmpty())
        {
            OutError = TEXT("destination_font_path must be a non-empty asset path.");
            return false;
        }

        int32 ColonIndex = INDEX_NONE;
        if (Resolved.FindChar(TEXT(':'), ColonIndex))
        {
            Resolved = Resolved.Left(ColonIndex);
        }

        int32 DotIndex = INDEX_NONE;
        if (Resolved.FindChar(TEXT('.'), DotIndex) && DotIndex > 0)
        {
            OutPackagePath = Resolved.Left(DotIndex);
            OutAssetName = Resolved.Mid(DotIndex + 1);
        }
        else
        {
            OutPackagePath = Resolved;
            OutAssetName = FPackageName::GetLongPackageAssetName(OutPackagePath);
        }

        if (OutAssetName.IsEmpty())
        {
            OutError = FString::Printf(TEXT("Could not derive asset name from '%s'."), *InPath);
            return false;
        }

        if (const FString ValidationError = MonolithCore::ValidatePackagePath(OutPackagePath); !ValidationError.IsEmpty())
        {
            OutError = ValidationError;
            return false;
        }

        return true;
    }

    static bool ParseRemapRulesWithExactMap(
        const TSharedPtr<FJsonObject>& Params,
        const FString& ExactMapFieldName,
        const FString& ExactMapDescription,
        TArray<FRemapRule>& OutRules,
        FString& OutError)
    {
        OutRules.Reset();
        OutError.Reset();

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

        SourceRoot = NormalizeCopyRepairPath(SourceRoot);
        DestRoot = NormalizeCopyRepairPath(DestRoot);
        if (SourceRoot.IsEmpty() != DestRoot.IsEmpty())
        {
            OutError = TEXT("source_root and dest_root must be supplied together.");
            return false;
        }
        if (!SourceRoot.IsEmpty())
        {
            if (!IsValidCopyRepairRoot(SourceRoot) || !IsValidCopyRepairRoot(DestRoot))
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
                Rule.From = NormalizeCopyRepairPath(Pair.Key);
                Rule.To = NormalizeCopyRepairPath(TargetRoot);
                if (!IsValidCopyRepairRoot(Rule.From) || !IsValidCopyRepairRoot(Rule.To))
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

        const TSharedPtr<FJsonObject>* ExactRemaps = nullptr;
        if (Params.IsValid() && Params->TryGetObjectField(ExactMapFieldName, ExactRemaps) && ExactRemaps)
        {
            for (const TPair<FString, TSharedPtr<FJsonValue>>& Pair : FMonolithJsonUtils::GetFields(*ExactRemaps))
            {
                FString TargetPath;
                if (!Pair.Value.IsValid() || !Pair.Value->TryGetString(TargetPath))
                {
                    OutError = FString::Printf(TEXT("%s['%s'] must be a string target %s path."), *ExactMapFieldName, *Pair.Key, *ExactMapDescription);
                    return false;
                }

                FRemapRule Rule;
                Rule.From = NormalizeCopyRepairPath(Pair.Key);
                Rule.To = NormalizeCopyRepairPath(TargetPath);
                if (Rule.From.IsEmpty() || Rule.To.IsEmpty())
                {
                    OutError = FString::Printf(TEXT("%s cannot contain empty source or target paths."), *ExactMapFieldName);
                    return false;
                }
                OutRules.Add(MoveTemp(Rule));
            }
        }

        OutRules.Sort([](const FRemapRule& A, const FRemapRule& B)
        {
            return A.From.Len() > B.From.Len();
        });

        if (OutRules.Num() == 0)
        {
            OutError = FString::Printf(TEXT("Provide root_remaps, source_root+dest_root, and/or %s so copied font references can be remapped."), *ExactMapFieldName);
            return false;
        }

        return true;
    }

    static bool ParseRemapRules(const TSharedPtr<FJsonObject>& Params, TArray<FRemapRule>& OutRules, FString& OutError)
    {
        return ParseRemapRulesWithExactMap(
            Params,
            TEXT("font_face_remaps"),
            TEXT("UFontFace asset"),
            OutRules,
            OutError);
    }

    static FFaceChange AnalyzeTypefaceEntry(
        const FString& Section,
        const FTypefaceEntry& Entry,
        const TArray<FRemapRule>& Rules)
    {
        FFaceChange Change;
        Change.Section = Section;
        Change.Typeface = Entry.Name.ToString();
        Change.SubFaceIndex = Entry.Font.GetSubFaceIndex();

        const UObject* FaceAsset = Entry.Font.GetFontFaceAsset();
        if (!FaceAsset)
        {
            Change.Status = TEXT("unchanged_no_font_face_asset");
            return Change;
        }

        Change.FromPath = NormalizeCopyRepairPath(FaceAsset->GetPathName());
        FString RemappedPath;
        FString MatchedFrom;
        if (!TryRemapPath(Change.FromPath, Rules, RemappedPath, MatchedFrom) || RemappedPath == Change.FromPath)
        {
            Change.ToPath = Change.FromPath;
            Change.Status = TEXT("unchanged");
            return Change;
        }

        Change.ToPath = NormalizeCopyRepairPath(RemappedPath);
        Change.MatchedRule = MatchedFrom;
        Change.TargetFace = FMonolithAssetUtils::LoadAssetByPath<UFontFace>(Change.ToPath);
        if (!Change.TargetFace)
        {
            Change.Status = TEXT("error");
            Change.Error = FString::Printf(
                TEXT("Remapped font face target '%s' could not be loaded as UFontFace."),
                *Change.ToPath);
            return Change;
        }

        Change.Status = TEXT("planned");
        return Change;
    }

    static void AnalyzeTypeface(
        const FString& Section,
        const FTypeface& Typeface,
        const TArray<FRemapRule>& Rules,
        TArray<FFaceChange>& OutChanges)
    {
        for (const FTypefaceEntry& Entry : Typeface.Fonts)
        {
            OutChanges.Add(AnalyzeTypefaceEntry(Section, Entry, Rules));
        }
    }

    static void ApplyTypefaceChanges(
        const FString& Section,
        FTypeface& Typeface,
        const TArray<FFaceChange>& Changes)
    {
        for (FTypefaceEntry& Entry : Typeface.Fonts)
        {
            const FFaceChange* MatchingChange = Changes.FindByPredicate(
                [&Section, &Entry](const FFaceChange& Candidate)
                {
                    return Candidate.Section == Section && Candidate.Typeface == Entry.Name.ToString();
                });

            if (MatchingChange && MatchingChange->TargetFace && MatchingChange->IsRemap())
            {
                Entry.Font = FFontData(MatchingChange->TargetFace, MatchingChange->SubFaceIndex);
            }
        }
    }

    struct FSlateFontRepairOptions
    {
        bool bApply = false;
        bool bIncludeUnchanged = false;
    };

    struct FSlateFontReferenceChange
    {
        FString ObjectPath;
        FString ObjectClass;
        FString PropertyPath;
        FString FromPath;
        FString ToPath;
        FString MatchedRule;
        FString Status;
        FString Error;
        FString Typeface;
        float Size = 0.0f;

        bool IsError() const
        {
            return !Error.IsEmpty();
        }

        bool IsRemap() const
        {
            return !FromPath.IsEmpty() && !ToPath.IsEmpty() && FromPath != ToPath;
        }
    };

    struct FSlateFontRepairStats
    {
        TArray<TSharedPtr<FJsonValue>> Changes;
        TArray<TSharedPtr<FJsonValue>> Errors;
        TMap<FString, TWeakObjectPtr<UObject>> ChangedObjects;
        int32 ScannedObjects = 0;
        int32 PlannedRemaps = 0;
        int32 AppliedRemaps = 0;
        int32 UnchangedFontInfos = 0;
    };

    static TSharedPtr<FJsonObject> MakeSlateFontChangeJson(const FSlateFontReferenceChange& Change)
    {
        TSharedPtr<FJsonObject> Obj = MakeShared<FJsonObject>();
        Obj->SetStringField(TEXT("object_path"), Change.ObjectPath);
        Obj->SetStringField(TEXT("object_class"), Change.ObjectClass);
        Obj->SetStringField(TEXT("property_path"), Change.PropertyPath);
        Obj->SetStringField(TEXT("status"), Change.Status);
        Obj->SetNumberField(TEXT("font_size"), Change.Size);
        if (!Change.Typeface.IsEmpty())
        {
            Obj->SetStringField(TEXT("typeface"), Change.Typeface);
        }
        if (!Change.FromPath.IsEmpty())
        {
            Obj->SetStringField(TEXT("from"), Change.FromPath);
        }
        if (!Change.ToPath.IsEmpty())
        {
            Obj->SetStringField(TEXT("to"), Change.ToPath);
        }
        if (!Change.MatchedRule.IsEmpty())
        {
            Obj->SetStringField(TEXT("matched_rule"), Change.MatchedRule);
        }
        if (!Change.Error.IsEmpty())
        {
            Obj->SetStringField(TEXT("error"), Change.Error);
        }
        return Obj;
    }

    static bool IsSlateFontInfoStruct(const FStructProperty* StructProperty)
    {
        return StructProperty && StructProperty->Struct == FSlateFontInfo::StaticStruct();
    }

    static void RecordSlateFontChange(
        const FSlateFontReferenceChange& Change,
        const FSlateFontRepairOptions& Options,
        FSlateFontRepairStats& Stats)
    {
        if (Change.IsError())
        {
            Stats.Errors.Add(MakeShared<FJsonValueObject>(MakeSlateFontChangeJson(Change)));
            Stats.Changes.Add(MakeShared<FJsonValueObject>(MakeSlateFontChangeJson(Change)));
            return;
        }

        if (Change.IsRemap())
        {
            if (Options.bApply)
            {
                Stats.AppliedRemaps++;
            }
            else
            {
                Stats.PlannedRemaps++;
            }
            Stats.Changes.Add(MakeShared<FJsonValueObject>(MakeSlateFontChangeJson(Change)));
            return;
        }

        Stats.UnchangedFontInfos++;
        if (Options.bIncludeUnchanged)
        {
            Stats.Changes.Add(MakeShared<FJsonValueObject>(MakeSlateFontChangeJson(Change)));
        }
    }

    static bool RepairSlateFontInfoValue(
        UObject* Owner,
        const FString& PropertyPath,
        FSlateFontInfo* FontInfo,
        const TArray<FRemapRule>& Rules,
        const FSlateFontRepairOptions& Options,
        FSlateFontRepairStats& Stats)
    {
        if (!Owner || !FontInfo)
        {
            return false;
        }

        FSlateFontReferenceChange Change;
        Change.ObjectPath = Owner->GetPathName();
        Change.ObjectClass = Owner->GetClass() ? Owner->GetClass()->GetPathName() : FString();
        Change.PropertyPath = PropertyPath;
        Change.Typeface = FontInfo->TypefaceFontName.ToString();
        Change.Size = FontInfo->Size;

        const UObject* CurrentFontObject = FontInfo->FontObject.Get();
        if (!CurrentFontObject)
        {
            Change.Status = TEXT("unchanged_no_font_object");
            RecordSlateFontChange(Change, Options, Stats);
            return false;
        }

        Change.FromPath = NormalizeCopyRepairPath(CurrentFontObject->GetPathName());
        FString RemappedPath;
        FString MatchedFrom;
        if (!TryRemapPath(Change.FromPath, Rules, RemappedPath, MatchedFrom) || RemappedPath == Change.FromPath)
        {
            Change.ToPath = Change.FromPath;
            Change.Status = TEXT("unchanged");
            RecordSlateFontChange(Change, Options, Stats);
            return false;
        }

        Change.ToPath = NormalizeCopyRepairPath(RemappedPath);
        Change.MatchedRule = MatchedFrom;
        UFont* TargetFont = FMonolithAssetUtils::LoadAssetByPath<UFont>(Change.ToPath);
        if (!TargetFont)
        {
            Change.Status = TEXT("error");
            Change.Error = FString::Printf(
                TEXT("Remapped Slate font target '%s' could not be loaded as UFont."),
                *Change.ToPath);
            RecordSlateFontChange(Change, Options, Stats);
            return false;
        }

        if (Options.bApply)
        {
            const FString ObjectPath = Owner->GetPathName();
            if (!Stats.ChangedObjects.Contains(ObjectPath))
            {
                Owner->Modify();
                Stats.ChangedObjects.Add(ObjectPath, Owner);
            }
            FontInfo->FontObject = TargetFont;
            Owner->MarkPackageDirty();
            Change.Status = TEXT("remapped");
        }
        else
        {
            Change.Status = TEXT("planned");
        }

        RecordSlateFontChange(Change, Options, Stats);
        return true;
    }

    static bool RepairSlateFontPropertyValue(
        FProperty* Property,
        void* ValuePtr,
        UObject* Owner,
        const FString& PropertyPath,
        const TArray<FRemapRule>& Rules,
        const FSlateFontRepairOptions& Options,
        FSlateFontRepairStats& Stats);

    static bool RepairSlateFontStructProperties(
        UStruct* Struct,
        void* StructValuePtr,
        UObject* Owner,
        const FString& Prefix,
        const TArray<FRemapRule>& Rules,
        const FSlateFontRepairOptions& Options,
        FSlateFontRepairStats& Stats)
    {
        bool bChanged = false;
        if (!Struct || !StructValuePtr)
        {
            return false;
        }

        for (TFieldIterator<FProperty> It(Struct); It; ++It)
        {
            FProperty* ChildProperty = *It;
            if (!ChildProperty || ChildProperty->HasAnyPropertyFlags(CPF_Transient | CPF_Deprecated))
            {
                continue;
            }

            void* ChildValuePtr = ChildProperty->ContainerPtrToValuePtr<void>(StructValuePtr);
            const FString ChildPath = Prefix.IsEmpty()
                ? ChildProperty->GetName()
                : Prefix + TEXT(".") + ChildProperty->GetName();
            bChanged |= RepairSlateFontPropertyValue(
                ChildProperty,
                ChildValuePtr,
                Owner,
                ChildPath,
                Rules,
                Options,
                Stats);
        }
        return bChanged;
    }

    static bool RepairSlateFontPropertyValue(
        FProperty* Property,
        void* ValuePtr,
        UObject* Owner,
        const FString& PropertyPath,
        const TArray<FRemapRule>& Rules,
        const FSlateFontRepairOptions& Options,
        FSlateFontRepairStats& Stats)
    {
        if (!Property || !ValuePtr)
        {
            return false;
        }

        if (FStructProperty* StructProperty = CastField<FStructProperty>(Property))
        {
            if (IsSlateFontInfoStruct(StructProperty))
            {
                return RepairSlateFontInfoValue(
                    Owner,
                    PropertyPath,
                    reinterpret_cast<FSlateFontInfo*>(ValuePtr),
                    Rules,
                    Options,
                    Stats);
            }
            return RepairSlateFontStructProperties(
                StructProperty->Struct,
                ValuePtr,
                Owner,
                PropertyPath,
                Rules,
                Options,
                Stats);
        }

        if (FArrayProperty* ArrayProperty = CastField<FArrayProperty>(Property))
        {
            bool bChanged = false;
            FScriptArrayHelper Helper(ArrayProperty, ValuePtr);
            for (int32 Index = 0; Index < Helper.Num(); ++Index)
            {
                bChanged |= RepairSlateFontPropertyValue(
                    ArrayProperty->Inner,
                    Helper.GetRawPtr(Index),
                    Owner,
                    FString::Printf(TEXT("%s[%d]"), *PropertyPath, Index),
                    Rules,
                    Options,
                    Stats);
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
                bChanged |= RepairSlateFontPropertyValue(
                    SetProperty->ElementProp,
                    Helper.GetElementPtr(Index),
                    Owner,
                    FString::Printf(TEXT("%s{%d}"), *PropertyPath, Index),
                    Rules,
                    Options,
                    Stats);
            }
            if (bChanged && Options.bApply)
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
                bChanged |= RepairSlateFontPropertyValue(
                    MapProperty->KeyProp,
                    Helper.GetKeyPtr(Index),
                    Owner,
                    FString::Printf(TEXT("%s{%d}.Key"), *PropertyPath, Index),
                    Rules,
                    Options,
                    Stats);
                bChanged |= RepairSlateFontPropertyValue(
                    MapProperty->ValueProp,
                    Helper.GetValuePtr(Index),
                    Owner,
                    FString::Printf(TEXT("%s{%d}.Value"), *PropertyPath, Index),
                    Rules,
                    Options,
                    Stats);
            }
            if (bChanged && Options.bApply)
            {
                Helper.Rehash();
            }
            return bChanged;
        }

        return false;
    }

    static bool ShouldScanSlateFontObject(UObject* Object, UPackage* Package)
    {
        return Object
            && Object->GetOutermost() == Package
            && !Object->IsA<UPackage>()
            && !Object->IsA<UClass>()
            && !Object->IsA<UWorld>()
            && !Object->HasAnyFlags(RF_ClassDefaultObject | RF_Transient);
    }

    static bool RepairSlateFontReferencesInObject(
        UObject* Object,
        const TArray<FRemapRule>& Rules,
        const FSlateFontRepairOptions& Options,
        FSlateFontRepairStats& Stats)
    {
        if (!Object)
        {
            return false;
        }

        Stats.ScannedObjects++;
        bool bChanged = false;
        for (TFieldIterator<FProperty> It(Object->GetClass(), EFieldIteratorFlags::IncludeSuper); It; ++It)
        {
            FProperty* Property = *It;
            if (!Property || Property->HasAnyPropertyFlags(CPF_Transient | CPF_Deprecated))
            {
                continue;
            }

            void* ValuePtr = Property->ContainerPtrToValuePtr<void>(Object);
            bChanged |= RepairSlateFontPropertyValue(
                Property,
                ValuePtr,
                Object,
                Property->GetName(),
                Rules,
                Options,
                Stats);
        }
        return bChanged;
    }

    static bool PackageContainsWorld(UPackage* Package)
    {
        if (!Package)
        {
            return false;
        }

        bool bContainsWorld = false;
        ForEachObjectWithPackage(Package, [&bContainsWorld](UObject* Object)
        {
            if (Object && Object->IsA<UWorld>())
            {
                bContainsWorld = true;
                return false;
            }
            return true;
        }, EGetObjectsFlags::IncludeNestedObjects);
        return bContainsWorld;
    }

    static void RepairSlateFontReferencesInPackage(
        UPackage* Package,
        const TArray<FRemapRule>& Rules,
        const FSlateFontRepairOptions& Options,
        FSlateFontRepairStats& Stats)
    {
        if (!Package)
        {
            return;
        }

        ForEachObjectWithPackage(Package, [&Rules, &Options, &Stats, Package](UObject* Object)
        {
            if (ShouldScanSlateFontObject(Object, Package))
            {
                RepairSlateFontReferencesInObject(Object, Rules, Options, Stats);
            }
            return true;
        }, EGetObjectsFlags::IncludeNestedObjects);
    }

    static bool SaveSlateFontRepairPackageIfRequested(UPackage* Package, bool bSave, FString& OutSavedFilename, FString& OutError)
    {
        OutSavedFilename.Reset();
        OutError.Reset();
        if (!bSave || !Package || !Package->IsDirty())
        {
            return true;
        }

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

void FMonolithUIFontRepairActions::RegisterActions(FMonolithToolRegistry& Registry)
{
    Registry.RegisterAction(
        TEXT("ui"),
        TEXT("clone_composite_font_with_remapped_faces"),
        TEXT("Clone a composite UFont to destination_font_path and remap its UFontFace references using root_remaps/source_root+dest_root and/or exact font_face_remaps. Dry-run is the default; mutating calls require confirm=true."),
        FMonolithActionHandler::CreateStatic(&HandleCloneCompositeFontWithRemappedFaces),
        FParamSchemaBuilder()
            .RequiredAssetPath(TEXT("source_font_path"), TEXT("Source composite UFont asset path"))
            .RequiredAssetPath(TEXT("destination_font_path"), TEXT("Destination UFont asset path to create"))
            .Optional(TEXT("root_remaps"), TEXT("object"), TEXT("Map of source package roots to destination roots, e.g. {\"/Game/Old\":\"/Game/New\"}"))
            .Optional(TEXT("source_root"), TEXT("string"), TEXT("Single source root shorthand; must be supplied with dest_root"))
            .Optional(TEXT("dest_root"), TEXT("string"), TEXT("Single destination root shorthand; must be supplied with source_root"))
            .Optional(TEXT("font_face_remaps"), TEXT("object"), TEXT("Exact source font face path to destination font face path map"))
            .Optional(TEXT("dry_run"), TEXT("boolean"), TEXT("Plan the clone/remap without creating the destination asset"), TEXT("true"))
            .Optional(TEXT("confirm"), TEXT("boolean"), TEXT("Required true when dry_run=false"), TEXT("false"))
            .Optional(TEXT("save"), TEXT("boolean"), TEXT("Save the new UFont package after creation"), TEXT("false"))
            .Build(),
        TEXT("PostCopyRepair")
    );

    Registry.RegisterAction(
        TEXT("ui"),
        TEXT("repair_slate_font_references"),
        TEXT("Repair FSlateFontInfo FontObject references inside a copied UI asset package using root_remaps/source_root+dest_root and/or exact font_asset_remaps. Dry-run is the default; mutating calls require confirm=true."),
        FMonolithActionHandler::CreateStatic(&HandleRepairSlateFontReferences),
        FParamSchemaBuilder()
            .RequiredAssetPath(TEXT("asset_path"), TEXT("Asset package to scan for serialized FSlateFontInfo values"))
            .Optional(TEXT("root_remaps"), TEXT("object"), TEXT("Map of source package roots to destination roots, e.g. {\"/Game/Old\":\"/Game/New\"}"))
            .Optional(TEXT("source_root"), TEXT("string"), TEXT("Single source root shorthand; must be supplied with dest_root"))
            .Optional(TEXT("dest_root"), TEXT("string"), TEXT("Single destination root shorthand; must be supplied with source_root"))
            .Optional(TEXT("font_asset_remaps"), TEXT("object"), TEXT("Exact source UFont asset path to destination UFont asset path map"))
            .Optional(TEXT("include_unchanged"), TEXT("boolean"), TEXT("Include unchanged FSlateFontInfo entries in the changes array"), TEXT("false"))
            .Optional(TEXT("dry_run"), TEXT("boolean"), TEXT("Plan the repair without mutating the asset package"), TEXT("true"))
            .Optional(TEXT("confirm"), TEXT("boolean"), TEXT("Required true when dry_run=false"), TEXT("false"))
            .Optional(TEXT("save"), TEXT("boolean"), TEXT("Save the package after applying changes"), TEXT("false"))
            .Build(),
        TEXT("PostCopyRepair")
    );

    FMonolithToolRegistry::Get().SetActionSearchMetadata(
        TEXT("ui"),
        TEXT("clone_composite_font_with_remapped_faces"),
        { TEXT("composite font remap"), TEXT("copied UI font repair"), TEXT("UFontFace remap"), TEXT("post-copy UI repair"), TEXT("Slate font copy") },
        { TEXT("repair_composite_font_faces"), TEXT("clone_font_with_remap"), TEXT("fix_copied_font_faces") },
        { TEXT("clone a copied composite UFont and remap /Game/Old font faces to /Game/New"), TEXT("dry-run composite font face remap before creating a UI font asset") });

    FMonolithToolRegistry::Get().SetActionSearchMetadata(
        TEXT("ui"),
        TEXT("repair_slate_font_references"),
        { TEXT("Slate font reference repair"), TEXT("FSlateFontInfo"), TEXT("copied WBP font repair"), TEXT("post-copy UI repair"), TEXT("UFont remap") },
        { TEXT("fix_slate_font_refs"), TEXT("repair_wbp_fonts"), TEXT("remap_slate_font_info") },
        { TEXT("repair copied Widget Blueprint FSlateFontInfo FontObject references from /Game/Old to /Game/New"), TEXT("dry-run Slate font reference remap before saving a copied UI asset") });
}

FMonolithActionResult FMonolithUIFontRepairActions::HandleCloneCompositeFontWithRemappedFaces(const TSharedPtr<FJsonObject>& Params)
{
    using namespace MonolithUI::FontRepair;

    FString SourceFontPath;
    FString ErrorMsg;
    if (!MonolithParamUtils::GetRequiredStringParam(Params, TEXT("source_font_path"), SourceFontPath, ErrorMsg))
    {
        return FMonolithActionResult::Error(ErrorMsg, FMonolithJsonUtils::ErrInvalidParams);
    }

    FString DestinationFontPath;
    if (!MonolithParamUtils::GetRequiredStringParam(Params, TEXT("destination_font_path"), DestinationFontPath, ErrorMsg))
    {
        return FMonolithActionResult::Error(ErrorMsg, FMonolithJsonUtils::ErrInvalidParams);
    }

    bool bDryRun = true;
    if (!MonolithParamUtils::GetOptionalBoolParam(Params, TEXT("dry_run"), bDryRun, ErrorMsg, true))
    {
        return FMonolithActionResult::Error(ErrorMsg, FMonolithJsonUtils::ErrInvalidParams);
    }

    bool bConfirm = false;
    if (!MonolithParamUtils::GetOptionalBoolParam(Params, TEXT("confirm"), bConfirm, ErrorMsg, false))
    {
        return FMonolithActionResult::Error(ErrorMsg, FMonolithJsonUtils::ErrInvalidParams);
    }
    if (!bDryRun && !bConfirm)
    {
        return FMonolithActionResult::Error(
            TEXT("clone_composite_font_with_remapped_faces is mutating; pass dry_run=true to inspect the plan or confirm=true with dry_run=false to apply."),
            FMonolithJsonUtils::ErrInvalidParams);
    }

    bool bSave = false;
    if (!MonolithParamUtils::GetOptionalBoolParam(Params, TEXT("save"), bSave, ErrorMsg, false))
    {
        return FMonolithActionResult::Error(ErrorMsg, FMonolithJsonUtils::ErrInvalidParams);
    }

    TArray<FRemapRule> Rules;
    if (!ParseRemapRules(Params, Rules, ErrorMsg))
    {
        return FMonolithActionResult::Error(ErrorMsg, FMonolithJsonUtils::ErrInvalidParams);
    }

    FString DestinationPackagePath;
    FString DestinationAssetName;
    if (!NormalizeAssetPackagePath(DestinationFontPath, DestinationPackagePath, DestinationAssetName, ErrorMsg))
    {
        return FMonolithActionResult::Error(ErrorMsg, FMonolithJsonUtils::ErrInvalidParams);
    }

    IAssetRegistry& AssetRegistry = FModuleManager::LoadModuleChecked<FAssetRegistryModule>(TEXT("AssetRegistry")).Get();
    const bool bDestinationExists = PackageOrAssetExists(AssetRegistry, DestinationPackagePath, DestinationAssetName);

    UFont* SourceFont = FMonolithAssetUtils::LoadAssetByPath<UFont>(SourceFontPath);
    if (!SourceFont)
    {
        return FMonolithActionResult::Error(
            FString::Printf(TEXT("Failed to load source_font_path '%s' as UFont."), *SourceFontPath),
            FMonolithJsonUtils::ErrInvalidParams);
    }

    const FCompositeFont& SourceComposite = SourceFont->GetInternalCompositeFont();
    TArray<FFaceChange> Changes;
    AnalyzeTypeface(TEXT("DefaultTypeface"), SourceComposite.DefaultTypeface, Rules, Changes);
    AnalyzeTypeface(TEXT("FallbackTypeface"), SourceComposite.FallbackTypeface.Typeface, Rules, Changes);
    for (int32 Index = 0; Index < SourceComposite.SubTypefaces.Num(); ++Index)
    {
        AnalyzeTypeface(
            FString::Printf(TEXT("SubTypefaces[%d]"), Index),
            SourceComposite.SubTypefaces[Index].Typeface,
            Rules,
            Changes);
    }

    TArray<TSharedPtr<FJsonValue>> ChangeArr;
    TArray<TSharedPtr<FJsonValue>> ErrorArr;
    int32 PlannedRemaps = 0;
    int32 UnchangedFaces = 0;
    for (const FFaceChange& Change : Changes)
    {
        FFaceChange SerializableChange = Change;
        if (SerializableChange.Status == TEXT("planned") && !bDryRun)
        {
            SerializableChange.Status = TEXT("pending");
        }
        if (SerializableChange.IsError())
        {
            ErrorArr.Add(MakeShared<FJsonValueObject>(MakeFaceChangeJson(SerializableChange)));
        }
        else if (SerializableChange.IsRemap())
        {
            PlannedRemaps++;
        }
        else
        {
            UnchangedFaces++;
        }
        ChangeArr.Add(MakeShared<FJsonValueObject>(MakeFaceChangeJson(SerializableChange)));
    }

    if (bDestinationExists)
    {
        TSharedPtr<FJsonObject> Collision = MakeShared<FJsonObject>();
        Collision->SetStringField(TEXT("section"), TEXT("Destination"));
        Collision->SetStringField(TEXT("status"), TEXT("error"));
        Collision->SetStringField(TEXT("error"), FString::Printf(
            TEXT("Destination asset '%s.%s' already exists; this action never overwrites existing font assets."),
            *DestinationPackagePath,
            *DestinationAssetName));
        ErrorArr.Add(MakeShared<FJsonValueObject>(Collision));
    }

    TArray<FString> RuleSummaries;
    RuleSummaries.Reserve(Rules.Num());
    for (const FRemapRule& Rule : Rules)
    {
        RuleSummaries.Add(Rule.From + TEXT(" -> ") + Rule.To);
    }

    auto MakeResult = [&]() -> TSharedPtr<FJsonObject>
    {
        TSharedPtr<FJsonObject> ResultJson = MakeShared<FJsonObject>();
        ResultJson->SetStringField(TEXT("source_font_path"), SourceFont->GetPathName());
        ResultJson->SetStringField(TEXT("destination_font_path"), DestinationPackagePath);
        ResultJson->SetBoolField(TEXT("dry_run"), bDryRun);
        ResultJson->SetBoolField(TEXT("destination_exists"), bDestinationExists);
        ResultJson->SetNumberField(TEXT("planned_remaps"), PlannedRemaps);
        ResultJson->SetNumberField(TEXT("unchanged_faces"), UnchangedFaces);
        ResultJson->SetNumberField(TEXT("error_count"), ErrorArr.Num());
        ResultJson->SetBoolField(TEXT("can_apply"), ErrorArr.Num() == 0);
        ResultJson->SetArrayField(TEXT("changes"), ChangeArr);
        ResultJson->SetArrayField(TEXT("errors"), ErrorArr);
        AddStringArrayField(ResultJson, TEXT("rules"), RuleSummaries);
        return ResultJson;
    };

    if (!bDryRun && ErrorArr.Num() > 0)
    {
        return FMonolithActionResult::Error(
            TEXT("Composite font clone/remap preflight failed; no asset was created."),
            FMonolithJsonUtils::ErrInvalidParams).WithErrorData(MakeResult());
    }

    if (bDryRun)
    {
        TSharedPtr<FJsonObject> ResultJson = MakeResult();
        ResultJson->SetBoolField(TEXT("created"), false);
        ResultJson->SetBoolField(TEXT("saved"), false);
        return FMonolithActionResult::Success(ResultJson);
    }

    if (const FString ValidationError = MonolithCore::ValidatePackagePath(DestinationPackagePath); !ValidationError.IsEmpty())
    {
        return FMonolithActionResult::Error(ValidationError, FMonolithJsonUtils::ErrInvalidParams);
    }

    UPackage* DestinationPackage = CreatePackage(*DestinationPackagePath);
    if (!DestinationPackage)
    {
        return FMonolithActionResult::Error(
            FString::Printf(TEXT("CreatePackage failed for '%s'."), *DestinationPackagePath),
            FMonolithJsonUtils::ErrInternalError);
    }
    // uses CreatePackage's return value directly with no FullyLoad — calling
    // FullyLoad on the in-memory hit path forces a serialization read that
    // can pull stale RF_Transient flags from a leftover .uasset into the
    // in-memory package.

    TUniquePtr<FScopedTransaction> Transaction;
    if (GEditor)
    {
        Transaction = MakeUnique<FScopedTransaction>(
            NSLOCTEXT("MonolithUI", "CloneCompositeFontWithRemappedFaces", "Clone Composite Font With Remapped Faces"));
    }

    UFont* NewFont = DuplicateObject<UFont>(SourceFont, DestinationPackage, FName(*DestinationAssetName));
    if (!NewFont)
    {
        return FMonolithActionResult::Error(
            FString::Printf(TEXT("DuplicateObject<UFont> failed for '%s'."), *DestinationPackagePath),
            FMonolithJsonUtils::ErrInternalError);
    }
    NewFont->SetFlags(RF_Public | RF_Standalone);
    NewFont->Modify();
    NewFont->LegacyFontName = FName(*DestinationAssetName);

    FCompositeFont& NewComposite = NewFont->GetMutableInternalCompositeFont();
    ApplyTypefaceChanges(TEXT("DefaultTypeface"), NewComposite.DefaultTypeface, Changes);
    ApplyTypefaceChanges(TEXT("FallbackTypeface"), NewComposite.FallbackTypeface.Typeface, Changes);
    for (int32 Index = 0; Index < NewComposite.SubTypefaces.Num(); ++Index)
    {
        ApplyTypefaceChanges(
            FString::Printf(TEXT("SubTypefaces[%d]"), Index),
            NewComposite.SubTypefaces[Index].Typeface,
            Changes);
    }

#if WITH_EDITORONLY_DATA
    NewComposite.MakeDirty();
#endif

    if (FSlateApplication::IsInitialized())
    {
        NewFont->PostEditChange();
    }
    FAssetRegistryModule::AssetCreated(NewFont);
    DestinationPackage->MarkPackageDirty();

    bool bSaved = false;
    if (bSave)
    {
        const FString PackageFilename = FPackageName::LongPackageNameToFilename(
            DestinationPackage->GetName(),
            FPackageName::GetAssetPackageExtension());

        FSavePackageArgs SaveArgs;
        SaveArgs.TopLevelFlags = RF_Public | RF_Standalone;
        SaveArgs.SaveFlags = SAVE_NoError;
        bSaved = UPackage::SavePackage(DestinationPackage, NewFont, *PackageFilename, SaveArgs);
        if (!bSaved)
        {
            return FMonolithActionResult::Error(
                FString::Printf(TEXT("UPackage::SavePackage failed for '%s'."), *PackageFilename),
                FMonolithJsonUtils::ErrInternalError);
        }
    }

    TArray<TSharedPtr<FJsonValue>> FinalChangeArr;
    FinalChangeArr.Reserve(Changes.Num());
    for (const FFaceChange& Change : Changes)
    {
        FFaceChange FinalChange = Change;
        if (FinalChange.Status == TEXT("planned"))
        {
            FinalChange.Status = TEXT("remapped");
        }
        FinalChangeArr.Add(MakeShared<FJsonValueObject>(MakeFaceChangeJson(FinalChange)));
    }

    TSharedPtr<FJsonObject> ResultJson = MakeResult();
    ResultJson->SetArrayField(TEXT("changes"), FinalChangeArr);
    ResultJson->SetBoolField(TEXT("created"), true);
    ResultJson->SetBoolField(TEXT("saved"), bSaved);
    return FMonolithActionResult::Success(ResultJson);
}

FMonolithActionResult FMonolithUIFontRepairActions::HandleRepairSlateFontReferences(const TSharedPtr<FJsonObject>& Params)
{
    using namespace MonolithUI::FontRepair;

    FString AssetPath;
    FString ErrorMsg;
    if (!MonolithParamUtils::GetRequiredStringParam(Params, TEXT("asset_path"), AssetPath, ErrorMsg))
    {
        return FMonolithActionResult::Error(ErrorMsg, FMonolithJsonUtils::ErrInvalidParams);
    }

    bool bDryRun = true;
    if (!MonolithParamUtils::GetOptionalBoolParam(Params, TEXT("dry_run"), bDryRun, ErrorMsg, true))
    {
        return FMonolithActionResult::Error(ErrorMsg, FMonolithJsonUtils::ErrInvalidParams);
    }

    bool bConfirm = false;
    if (!MonolithParamUtils::GetOptionalBoolParam(Params, TEXT("confirm"), bConfirm, ErrorMsg, false))
    {
        return FMonolithActionResult::Error(ErrorMsg, FMonolithJsonUtils::ErrInvalidParams);
    }
    if (!bDryRun && !bConfirm)
    {
        return FMonolithActionResult::Error(
            TEXT("repair_slate_font_references is mutating; pass dry_run=true to inspect the plan or confirm=true with dry_run=false to apply."),
            FMonolithJsonUtils::ErrInvalidParams);
    }

    bool bSave = false;
    if (!MonolithParamUtils::GetOptionalBoolParam(Params, TEXT("save"), bSave, ErrorMsg, false))
    {
        return FMonolithActionResult::Error(ErrorMsg, FMonolithJsonUtils::ErrInvalidParams);
    }

    bool bIncludeUnchanged = false;
    if (!MonolithParamUtils::GetOptionalBoolParam(Params, TEXT("include_unchanged"), bIncludeUnchanged, ErrorMsg, false))
    {
        return FMonolithActionResult::Error(ErrorMsg, FMonolithJsonUtils::ErrInvalidParams);
    }

    TArray<FRemapRule> Rules;
    if (!ParseRemapRulesWithExactMap(Params, TEXT("font_asset_remaps"), TEXT("UFont asset"), Rules, ErrorMsg))
    {
        return FMonolithActionResult::Error(ErrorMsg, FMonolithJsonUtils::ErrInvalidParams);
    }

    UObject* Asset = FMonolithAssetUtils::LoadAssetByPath<UObject>(AssetPath);
    if (!Asset)
    {
        return FMonolithActionResult::Error(
            FString::Printf(TEXT("Failed to load asset_path '%s'."), *AssetPath),
            FMonolithJsonUtils::ErrInvalidParams);
    }

    UPackage* Package = Asset->GetOutermost();
    if (!Package)
    {
        return FMonolithActionResult::Error(
            FString::Printf(TEXT("Loaded asset '%s' has no outer package."), *Asset->GetPathName()),
            FMonolithJsonUtils::ErrInternalError);
    }
    Package->FullyLoad();

    if (PackageContainsWorld(Package))
    {
        return FMonolithActionResult::Error(
            FString::Printf(TEXT("repair_slate_font_references only supports non-map asset packages; '%s' contains a UWorld."), *Package->GetName()),
            FMonolithJsonUtils::ErrInvalidParams);
    }

    TArray<FString> RuleSummaries;
    RuleSummaries.Reserve(Rules.Num());
    for (const FRemapRule& Rule : Rules)
    {
        RuleSummaries.Add(Rule.From + TEXT(" -> ") + Rule.To);
    }

    auto MakeResult = [&](const FSlateFontRepairStats& Stats, bool bSaved, const FString& SavedFilename) -> TSharedPtr<FJsonObject>
    {
        TSharedPtr<FJsonObject> ResultJson = MakeShared<FJsonObject>();
        ResultJson->SetStringField(TEXT("asset_path"), Asset->GetPathName());
        ResultJson->SetStringField(TEXT("package_path"), Package->GetName());
        ResultJson->SetBoolField(TEXT("dry_run"), bDryRun);
        ResultJson->SetBoolField(TEXT("saved"), bSaved);
        if (!SavedFilename.IsEmpty())
        {
            ResultJson->SetStringField(TEXT("saved_filename"), SavedFilename);
        }
        ResultJson->SetNumberField(TEXT("scanned_objects"), Stats.ScannedObjects);
        ResultJson->SetNumberField(TEXT("planned_remaps"), Stats.PlannedRemaps);
        ResultJson->SetNumberField(TEXT("applied_remaps"), Stats.AppliedRemaps);
        ResultJson->SetNumberField(TEXT("unchanged_font_infos"), Stats.UnchangedFontInfos);
        ResultJson->SetNumberField(TEXT("error_count"), Stats.Errors.Num());
        ResultJson->SetNumberField(TEXT("changed_object_count"), Stats.ChangedObjects.Num());
        ResultJson->SetBoolField(TEXT("can_apply"), Stats.Errors.Num() == 0);
        ResultJson->SetArrayField(TEXT("changes"), Stats.Changes);
        ResultJson->SetArrayField(TEXT("errors"), Stats.Errors);
        AddStringArrayField(ResultJson, TEXT("rules"), RuleSummaries);

        TArray<FString> ChangedObjectPaths;
        Stats.ChangedObjects.GetKeys(ChangedObjectPaths);
        ChangedObjectPaths.Sort();
        AddStringArrayField(ResultJson, TEXT("changed_objects"), ChangedObjectPaths);
        return ResultJson;
    };

    FSlateFontRepairStats PreflightStats;
    FSlateFontRepairOptions PreflightOptions;
    PreflightOptions.bApply = false;
    PreflightOptions.bIncludeUnchanged = bIncludeUnchanged;
    RepairSlateFontReferencesInPackage(Package, Rules, PreflightOptions, PreflightStats);

    if (bDryRun)
    {
        return FMonolithActionResult::Success(MakeResult(PreflightStats, false, FString()));
    }

    if (PreflightStats.Errors.Num() > 0)
    {
        return FMonolithActionResult::Error(
            TEXT("Slate font reference repair preflight failed; no asset was modified."),
            FMonolithJsonUtils::ErrInvalidParams).WithErrorData(MakeResult(PreflightStats, false, FString()));
    }

    TUniquePtr<FScopedTransaction> Transaction;
    if (GEditor)
    {
        Transaction = MakeUnique<FScopedTransaction>(
            NSLOCTEXT("MonolithUI", "RepairSlateFontReferences", "Repair Slate Font References"));
    }

    FSlateFontRepairStats ApplyStats;
    FSlateFontRepairOptions ApplyOptions;
    ApplyOptions.bApply = true;
    ApplyOptions.bIncludeUnchanged = bIncludeUnchanged;
    RepairSlateFontReferencesInPackage(Package, Rules, ApplyOptions, ApplyStats);

    if (ApplyStats.Errors.Num() > 0)
    {
        return FMonolithActionResult::Error(
            TEXT("Slate font reference repair failed during apply; inspect error_data for partially applied objects."),
            FMonolithJsonUtils::ErrInternalError).WithErrorData(MakeResult(ApplyStats, false, FString()));
    }

#if WITH_EDITOR
    for (const TPair<FString, TWeakObjectPtr<UObject>>& Pair : ApplyStats.ChangedObjects)
    {
        if (UObject* ChangedObject = Pair.Value.Get())
        {
            ChangedObject->PostEditChange();
        }
    }
#endif

    if (ApplyStats.AppliedRemaps > 0)
    {
        Package->MarkPackageDirty();
    }

    FString SavedFilename;
    if (!SaveSlateFontRepairPackageIfRequested(Package, bSave && ApplyStats.AppliedRemaps > 0, SavedFilename, ErrorMsg))
    {
        return FMonolithActionResult::Error(
            ErrorMsg,
            FMonolithJsonUtils::ErrInternalError).WithErrorData(MakeResult(ApplyStats, false, FString()));
    }

    return FMonolithActionResult::Success(MakeResult(ApplyStats, !SavedFilename.IsEmpty(), SavedFilename));
}
