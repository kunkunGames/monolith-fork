// Copyright tumourlove. All Rights Reserved.

#include "Spec/ResidentWidgetUISpecFingerprint.h"

#include "MonolithHashUtils.h"
#include "Spec/UISpecJsonSerializer.h"
#include "Spec/UISpecSerializer.h"

#include "Blueprint/WidgetBlueprintGeneratedClass.h"
#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "Misc/PackageName.h"
#include "UObject/Package.h"
#include "WidgetBlueprint.h"

namespace MonolithUI::ResidentUISpecFingerprintInternal
{
    static void Fail(
        FMonolithUIResidentUISpecFingerprintResult& Result,
        const FString& Code,
        const FString& Reason,
        int32 InputIndex = INDEX_NONE)
    {
        Result.bSuccess = false;
        Result.AggregateSha256.Reset();
        Result.Widgets.Reset();
        Result.FailureCode = Code;
        Result.FailureReason = Reason;
        Result.FailedInputIndex = InputIndex;
    }

    static FString JoinFindings(const TArray<FUISpecError>& Findings)
    {
        TArray<FString> Messages;
        Messages.Reserve(Findings.Num());
        for (const FUISpecError& Finding : Findings)
        {
            Messages.Add(FString::Printf(
                TEXT("[%s] %s"),
                *Finding.Category.ToString(),
                *Finding.Message));
        }
        return FString::Join(Messages, TEXT("; "));
    }

    static bool IsLowercaseSha256(const FString& Value)
    {
        if (Value.Len() != 64)
        {
            return false;
        }
        for (const TCHAR Character : Value)
        {
            if (!FChar::IsDigit(Character) && (Character < TEXT('a') || Character > TEXT('f')))
            {
                return false;
            }
        }
        return true;
    }

    static bool TryHashCanonicalText(const FString& Text, FString& OutSha256, FString& OutError)
    {
        OutSha256.Reset();
        OutError.Reset();
        if (!FMonolithHashUtils::TrySha256Text(Text, OutSha256))
        {
            OutError = TEXT("The platform SHA-256 provider failed; no fallback digest is permitted.");
            return false;
        }
        OutSha256.ToLowerInline();
        if (!IsLowercaseSha256(OutSha256))
        {
            OutError = TEXT("The platform SHA-256 provider returned a non-canonical digest.");
            OutSha256.Reset();
            return false;
        }
        return true;
    }

    static bool HasUnresolvedObjectFlags(const UObject* Object)
    {
        return !Object || Object->HasAnyFlags(
            RF_NeedLoad | RF_NeedPostLoad | RF_BeginDestroyed | RF_FinishDestroyed);
    }
}

FMonolithUIResidentUISpecFingerprintResult FMonolithUIResidentUISpecFingerprint::Compute(
    TConstArrayView<UClass*> ResidentGeneratedClasses)
{
    using namespace MonolithUI::ResidentUISpecFingerprintInternal;

    FMonolithUIResidentUISpecFingerprintResult Result;
    if (!IsInGameThread())
    {
        Fail(Result, TEXT("NotOnGameThread"), TEXT("Resident UISpec fingerprinting must run on the editor game thread."));
        return Result;
    }
    if (ResidentGeneratedClasses.IsEmpty())
    {
        Fail(Result, TEXT("EmptyGeneratedClassSet"), TEXT("At least one resident Widget Blueprint generated class is required."));
        return Result;
    }

    struct FResolvedResidentWidget
    {
        UWidgetBlueprint* WidgetBlueprint = nullptr;
        UWidgetBlueprintGeneratedClass* GeneratedClass = nullptr;
        FString PackageName;
        int32 InputIndex = INDEX_NONE;
    };

    TArray<FResolvedResidentWidget> Resolved;
    Resolved.Reserve(ResidentGeneratedClasses.Num());
    TSet<FString> SeenPackages;

    for (int32 InputIndex = 0; InputIndex < ResidentGeneratedClasses.Num(); ++InputIndex)
    {
        UClass* const InputClass = ResidentGeneratedClasses[InputIndex];
        if (!IsValid(InputClass) || HasUnresolvedObjectFlags(InputClass))
        {
            Fail(
                Result,
                TEXT("GeneratedClassNotResident"),
                FString::Printf(TEXT("Input %d is null, invalid, pending load/post-load, or being destroyed."), InputIndex),
                InputIndex);
            return Result;
        }

        UWidgetBlueprintGeneratedClass* const GeneratedClass = Cast<UWidgetBlueprintGeneratedClass>(InputClass);
        if (!GeneratedClass)
        {
            Fail(
                Result,
                TEXT("NotWidgetBlueprintGeneratedClass"),
                FString::Printf(TEXT("Input %d ('%s') is not a UWidgetBlueprintGeneratedClass."), InputIndex, *InputClass->GetPathName()),
                InputIndex);
            return Result;
        }

        UWidgetBlueprint* const WidgetBlueprint = Cast<UWidgetBlueprint>(GeneratedClass->ClassGeneratedBy);
        if (!IsValid(WidgetBlueprint) || HasUnresolvedObjectFlags(WidgetBlueprint))
        {
            Fail(
                Result,
                TEXT("ClassGeneratedByNotResidentWidgetBlueprint"),
                FString::Printf(
                    TEXT("Generated class '%s' has no valid resident UWidgetBlueprint in ClassGeneratedBy."),
                    *GeneratedClass->GetPathName()),
                InputIndex);
            return Result;
        }
        if (WidgetBlueprint->GeneratedClass != GeneratedClass)
        {
            Fail(
                Result,
                TEXT("GeneratedClassBackReferenceMismatch"),
                FString::Printf(
                    TEXT("Widget Blueprint '%s' does not point back to generated class '%s'."),
                    *WidgetBlueprint->GetPathName(),
                    *GeneratedClass->GetPathName()),
                InputIndex);
            return Result;
        }

		UPackage* const Package = WidgetBlueprint->GetOutermost();
		if (!Package
			|| Package == GetTransientPackage()
			|| WidgetBlueprint->HasAnyFlags(RF_Transient)
            || GeneratedClass->HasAnyFlags(RF_Transient)
            || GeneratedClass->GetOutermost() != Package)
        {
            Fail(
                Result,
                TEXT("TransientOrMismatchedPackage"),
                FString::Printf(
                    TEXT("Widget Blueprint/generated class pair '%s' / '%s' is transient or does not share one package."),
                    *WidgetBlueprint->GetPathName(),
                    *GeneratedClass->GetPathName()),
                InputIndex);
            return Result;
        }

        const FString PackageName = Package->GetName();
        if (!FPackageName::IsValidLongPackageName(PackageName, /*bIncludeReadOnlyRoots=*/true))
        {
            Fail(
                Result,
                TEXT("InvalidPackageName"),
                FString::Printf(TEXT("Widget Blueprint '%s' has invalid long package name '%s'."), *WidgetBlueprint->GetPathName(), *PackageName),
                InputIndex);
            return Result;
        }
        if (SeenPackages.Contains(PackageName))
        {
            Fail(
                Result,
                TEXT("DuplicatePackage"),
                FString::Printf(TEXT("Resident generated-class set contains package '%s' more than once."), *PackageName),
                InputIndex);
            return Result;
        }
        SeenPackages.Add(PackageName);
        Resolved.Add({WidgetBlueprint, GeneratedClass, PackageName, InputIndex});
    }

    Resolved.Sort([](const FResolvedResidentWidget& A, const FResolvedResidentWidget& B)
    {
        return A.PackageName.Compare(B.PackageName, ESearchCase::CaseSensitive) < 0;
    });

    Result.Widgets.Reserve(Resolved.Num());
    for (const FResolvedResidentWidget& Entry : Resolved)
    {
        const FUISpecSerializerResult Dump = FUISpecSerializer::DumpFromWBP(
            Entry.WidgetBlueprint,
            Entry.PackageName);
        if (!Dump.bSuccess || Dump.Errors.Num() > 0 || !Dump.Document.Root.IsValid())
        {
            Fail(
                Result,
                TEXT("UISpecDumpFailed"),
                FString::Printf(
                    TEXT("UISpec dump failed for '%s': %s"),
                    *Entry.PackageName,
                    Dump.Errors.Num() > 0 ? *JoinFindings(Dump.Errors) : TEXT("serializer produced no root widget")),
                Entry.InputIndex);
            return Result;
        }

        FString CanonicalUISpec;
        FString CanonicalError;
        if (!FUISpecJsonSerializer::TryWriteCanonicalDocument(Dump.Document, CanonicalUISpec, CanonicalError))
        {
            Fail(
                Result,
                TEXT("UISpecCanonicalizationFailed"),
                FString::Printf(TEXT("UISpec canonicalization failed for '%s': %s"), *Entry.PackageName, *CanonicalError),
                Entry.InputIndex);
            return Result;
        }

        FMonolithUIResidentUISpecDigest Row;
        Row.PackageName = Entry.PackageName;
        Row.WidgetBlueprintPath = Entry.WidgetBlueprint->GetPathName();
        Row.GeneratedClassPath = Entry.GeneratedClass->GetPathName();
        Row.NodesVisited = Dump.NodesVisited;
        Row.AnimationsCaptured = Dump.AnimationsCaptured;

        FString HashError;
        if (!TryHashCanonicalText(CanonicalUISpec, Row.UISpecSha256, HashError))
        {
            Fail(
                Result,
                TEXT("UISpecSha256Failed"),
                FString::Printf(TEXT("UISpec SHA-256 failed for '%s': %s"), *Entry.PackageName, *HashError),
                Entry.InputIndex);
            return Result;
        }
        Result.Widgets.Add(MoveTemp(Row));
    }

    TSharedPtr<FJsonObject> Aggregate = MakeShared<FJsonObject>();
    Aggregate->SetStringField(TEXT("schema_version"), Result.SchemaVersion);
    TArray<TSharedPtr<FJsonValue>> WidgetRows;
    WidgetRows.Reserve(Result.Widgets.Num());
    for (const FMonolithUIResidentUISpecDigest& Row : Result.Widgets)
    {
        TSharedPtr<FJsonObject> JsonRow = MakeShared<FJsonObject>();
        JsonRow->SetStringField(TEXT("package_name"), Row.PackageName);
        JsonRow->SetStringField(TEXT("widget_blueprint_path"), Row.WidgetBlueprintPath);
        JsonRow->SetStringField(TEXT("generated_class_path"), Row.GeneratedClassPath);
        JsonRow->SetStringField(TEXT("ui_spec_sha256"), Row.UISpecSha256);
        WidgetRows.Add(MakeShared<FJsonValueObject>(JsonRow));
    }
    Aggregate->SetArrayField(TEXT("widgets"), WidgetRows);

    FString CanonicalAggregate;
    FString CanonicalError;
    if (!FUISpecJsonSerializer::TryWriteCanonicalJson(Aggregate, CanonicalAggregate, CanonicalError))
    {
        Fail(Result, TEXT("AggregateCanonicalizationFailed"), CanonicalError);
        return Result;
    }

    FString HashError;
    if (!TryHashCanonicalText(CanonicalAggregate, Result.AggregateSha256, HashError))
    {
        Fail(Result, TEXT("AggregateSha256Failed"), HashError);
        return Result;
    }

    Result.bSuccess = true;
    return Result;
}
