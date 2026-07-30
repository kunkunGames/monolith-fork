#pragma once

#include "CoreMinimal.h"

class UClass;

struct FMonolithPCGSettingsTypeInfo
{
	UClass* SettingsClass = nullptr;
	FString ClassName;
	FString ClassPath;
	FString FriendlyName;
	FString DefaultNodeTitle;
};

/**
 * Resolves user-facing PCG node type tokens to concrete UPCGSettings classes.
 *
 * The resolver deliberately derives its inventory from the live UE reflection
 * surface instead of maintaining a hand-written node table. This keeps the
 * action compatible with engine point releases and project-defined PCG node
 * settings while still rejecting abstract/deprecated classes and ambiguous
 * friendly names.
 */
class FMonolithPCGSettingsResolver
{
  public:
	static TArray<FMonolithPCGSettingsTypeInfo> ListTypes();

	static UClass* Resolve(const FString& TypeToken, TArray<FString>& OutCandidates, FString& OutError);

	static FString MakeFriendlyName(const UClass* SettingsClass);

  private:
	static FString Canonicalize(const FString& Value);
	static bool IsUsableSettingsClass(const UClass* Candidate);
};
