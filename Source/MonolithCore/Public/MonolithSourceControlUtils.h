#pragma once

#include "CoreMinimal.h"
#include "Dom/JsonObject.h"

class UObject;
class UPackage;

struct MONOLITHCORE_API FMonolithSourceControlPrepareOptions
{
	bool bDryRun = false;

	/** Treat an unavailable provider as a non-fatal skip. Use true for automatic asset mutation preparation. */
	bool bUnavailableIsSuccess = true;

	/** Allow mark-for-add attempts for files that do not exist yet. */
	bool bAddMissingFiles = false;
};

class MONOLITHCORE_API FMonolithSourceControlUtils
{
public:
	static bool IsProviderAvailable(FString& OutReason);
	static bool TryGetMountedPackageName(const FString& Input, FString& OutPackageName);
	static bool NormalizePathForSourceControl(const FString& Input, FString& OutFile, FString& OutError);
	static bool PackageNameToFilename(const FString& PackageName, FString& OutFile, FString& OutError);

	static TSharedPtr<FJsonObject> CheckoutOrAddFiles(
		const TArray<FString>& Inputs,
		const FMonolithSourceControlPrepareOptions& Options = FMonolithSourceControlPrepareOptions());

	static TSharedPtr<FJsonObject> CheckoutOrAddPackageNames(
		const TArray<FString>& PackageNames,
		const FMonolithSourceControlPrepareOptions& Options = FMonolithSourceControlPrepareOptions());

	static TSharedPtr<FJsonObject> CheckoutOrAddPackage(
		UPackage* Package,
		const FMonolithSourceControlPrepareOptions& Options = FMonolithSourceControlPrepareOptions());

	static TSharedPtr<FJsonObject> CheckoutOrAddAsset(
		UObject* Asset,
		const FMonolithSourceControlPrepareOptions& Options = FMonolithSourceControlPrepareOptions());
};
