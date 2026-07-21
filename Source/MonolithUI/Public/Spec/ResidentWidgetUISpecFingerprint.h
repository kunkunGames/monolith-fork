// Copyright tumourlove. All Rights Reserved.
#pragma once

#include "CoreMinimal.h"

class UClass;

/** Per-resident-WBP identity row in a deterministic UISpec fingerprint. */
struct MONOLITHUI_API FMonolithUIResidentUISpecDigest
{
    /** Exact long package name, used as the primary sort key. */
    FString PackageName;

    /** Exact resident UWidgetBlueprint object path (`/Package.Asset`). */
    FString WidgetBlueprintPath;

    /** Exact resident UWidgetBlueprintGeneratedClass object path. */
    FString GeneratedClassPath;

    /** Lowercase SHA-256 of the canonical `ui.dump_ui_spec.spec` JSON. */
    FString UISpecSha256;

    int32 NodesVisited = 0;
    int32 AnimationsCaptured = 0;
};

/** Fail-closed result for one resident generated-class set. */
struct MONOLITHUI_API FMonolithUIResidentUISpecFingerprintResult
{
    bool bSuccess = false;
    FString SchemaVersion = TEXT("monolith_ui.resident_uispec_fingerprint.v1");

    /** Lowercase SHA-256 over the package-sorted per-WBP identity rows. */
    FString AggregateSha256;

    /** Package-sorted rows. Cleared on failure; never contains a partial set. */
    TArray<FMonolithUIResidentUISpecDigest> Widgets;

    FString FailureCode;
    FString FailureReason;
    int32 FailedInputIndex = INDEX_NONE;
};

/**
 * Computes UISpec identity from already-resident production generated classes.
 *
 * This utility never resolves paths or loads assets. Every input must be an
 * already-resident UWidgetBlueprintGeneratedClass whose ClassGeneratedBy is an
 * already-resident UWidgetBlueprint and whose blueprint GeneratedClass points
 * back to the exact input class. Null, native, transient, pending-load,
 * mismatched, or duplicate-package inputs fail the whole operation.
 */
class MONOLITHUI_API FMonolithUIResidentUISpecFingerprint
{
public:
    static FMonolithUIResidentUISpecFingerprintResult Compute(
        TConstArrayView<UClass*> ResidentGeneratedClasses);
};
