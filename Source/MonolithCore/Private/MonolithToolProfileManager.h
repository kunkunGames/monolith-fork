#pragma once

#include "CoreMinimal.h"
#include "Dom/JsonObject.h"
#include "MonolithToolRegistry.h"

struct FMonolithToolProfile
{
	FString Id;
	FString DisplayName;
	FString Description;
	FString Mode = TEXT("denylist");
	FString CustomInstructions;
	bool bBuiltIn = false;

	TSet<FString> EnabledNamespaces;
	TSet<FString> EnabledActions;
	TSet<FString> DisabledNamespaces;
	TSet<FString> DisabledActions;
	TMap<FString, FString> DescriptionOverrides;
};

class FMonolithToolProfileManager
{
public:
	static FMonolithToolProfileManager& Get();

	FString GetActiveProfileId();
	bool SetActiveProfile(const FString& ProfileId, FString& OutError);

	TArray<FMonolithToolProfile> ListProfiles();
	TOptional<FMonolithToolProfile> GetProfile(const FString& ProfileId);
	TSharedPtr<FJsonObject> ProfileToJson(const FMonolithToolProfile& Profile) const;

	bool UpsertProfile(const FMonolithToolProfile& Profile, bool bCreateOnly, FString& OutError);
	bool DeleteProfile(const FString& ProfileId, FString& OutError);
	bool SetActionEnabled(const FString& ProfileId, const FString& ActionId, bool bEnabled, FString& OutError);
	bool SetNamespaceEnabled(const FString& ProfileId, const FString& Namespace, bool bEnabled, FString& OutError);
	bool SetDescriptionOverride(const FString& ProfileId, const FString& ActionId, const FString& Description, FString& OutError);

	bool IsActionAllowed(const FString& Namespace, const FString& Action);
	FMonolithActionInfo ApplyDescriptionOverride(const FMonolithActionInfo& Info);
	bool ValidateProfile(const FString& ProfileId, TArray<FString>& OutUnknownNamespaces, TArray<FString>& OutUnknownActions, FString& OutError);

	/**
	 * Monotonic revision of the advertised tool list. Bumped whenever a mutator
	 * changes which actions/namespaces are visible or how they are described
	 * (active profile, action/namespace enablement, description override,
	 * upsert, delete). The MCP server reads this to advertise tools.listChanged
	 * and to track the list revision for a future notifications/tools/list_changed
	 * push. This manager stays MCP-agnostic — it owns only the counter.
	 */
	int64 GetToolListRevision() const;

private:
	FMonolithToolProfileManager() = default;

	void EnsureLoaded();
	void Load_NoLock();
	bool Save_NoLock(FString& OutError) const;
	void AddDefaultProfile_NoLock();
	void BumpToolListRevision_NoLock();
	static FString MakeActionId(const FString& Namespace, const FString& Action);
	static bool IsProfileManagementAction(const FString& Namespace, const FString& Action);

	mutable FCriticalSection Lock;
	bool bLoaded = false;
	FString ActiveProfileId = TEXT("default");
	TMap<FString, FMonolithToolProfile> Profiles;
	int64 ToolListRevision = 0;
};
