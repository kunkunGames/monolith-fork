#pragma once

#include "CoreMinimal.h"
#include "Dom/JsonObject.h"

/** Result of an action execution */
struct FMonolithActionResult
{
	bool bSuccess = false;
	TSharedPtr<FJsonObject> Result;
	FString ErrorMessage;
	int32 ErrorCode = 0;

	// CC-05: structured hint slots — empty by default → existing responses byte-identical.
	// RelatedActions: "did you mean" suggestions (action names, asset paths, etc.).
	// Hints: free-form follow-up guidance for the agent.
	// ErrorData: handler-supplied structured context, attached to JSON-RPC error.data.
	TArray<FString> RelatedActions;
	TArray<FString> Hints;
	TSharedPtr<FJsonObject> ErrorData;

	static FMonolithActionResult Success(const TSharedPtr<FJsonObject>& InResult)
	{
		FMonolithActionResult R;
		R.bSuccess = true;
		R.Result = InResult;
		return R;
	}

	static FMonolithActionResult Error(const FString& Message, int32 Code = -32603)
	{
		FMonolithActionResult R;
		R.bSuccess = false;
		R.ErrorMessage = Message;
		R.ErrorCode = Code;
		return R;
	}

	// Chain helpers — keep handler call sites readable:
	//   return FMonolithActionResult::Error("...").WithHint("...").WithRelatedAction("...");
	FMonolithActionResult& WithHint(const FString& Hint) { Hints.Add(Hint); return *this; }
	FMonolithActionResult& WithRelatedAction(const FString& Name) { RelatedActions.Add(Name); return *this; }
	FMonolithActionResult& WithRelatedActions(const TArray<FString>& Names)
	{
		RelatedActions.Append(Names);
		return *this;
	}
	FMonolithActionResult& WithErrorData(const TSharedPtr<FJsonObject>& Data) { ErrorData = Data; return *this; }

	// DD-02: structured asset-path recovery
	FMonolithActionResult& WithRetryWith(const TSharedPtr<FJsonObject>& Args)
	{
		if (!ErrorData.IsValid()) ErrorData = MakeShared<FJsonObject>();
		ErrorData->SetObjectField(TEXT("retry_with"), Args);
		return *this;
	}

	FMonolithActionResult& WithDidYouMean(const TArray<FString>& Candidates)
	{
		if (!ErrorData.IsValid()) ErrorData = MakeShared<FJsonObject>();
		
		TArray<TSharedPtr<FJsonValue>> Arr;
		Arr.Reserve(Candidates.Num());
		for (const FString& Cand : Candidates)
		{
			Arr.Add(MakeShared<FJsonValueString>(Cand));
		}
		ErrorData->SetArrayField(TEXT("did_you_mean"), Arr);
		return *this;
	}
};

/** Delegate type for action handlers */
DECLARE_DELEGATE_RetVal_OneParam(FMonolithActionResult, FMonolithActionHandler, const TSharedPtr<FJsonObject>& /* Params */);

/** First-slice registry metadata for future policy-driven execution guard behavior. */
struct MONOLITHCORE_API FMonolithActionExecutionPolicy
{
	FString PolicyId = TEXT("read_only");
	bool bDefaulted = true;
	bool bDirtyPackageTracking = false;
	bool bTransactionWrapping = false;
	bool bPostEditValidation = false;
	bool bEnforced = false;

	static FMonolithActionExecutionPolicy DefaultReadOnly();
	TSharedPtr<FJsonObject> ToJson() const;
};

/** Metadata describing a registered action */
struct FMonolithActionInfo
{
	FString Namespace;
	FString Action;
	FString Description;
	FString Category;                     // Optional sub-grouping within a namespace (e.g. "CommonUI" inside "ui"). Empty = uncategorized.
	FMonolithActionExecutionPolicy ExecutionPolicy;
	TSharedPtr<FJsonObject> ParamSchema;  // JSON Schema for parameter validation
};

/**
 * Central registry for all Monolith tool actions.
 * Domain modules register actions here. The HTTP server dispatches through this.
 */
class MONOLITHCORE_API FMonolithToolRegistry
{
public:
	static FMonolithToolRegistry& Get();

	/**
	 * Register an action handler.
	 * @param Namespace   The tool namespace (e.g., "blueprint", "material")
	 * @param Action      The action name (e.g., "list_graphs", "get_node")
	 * @param Description Human-readable description of what this action does
	 * @param Handler     The delegate to execute
	 * @param ParamSchema Optional JSON Schema describing expected parameters
	 */
	void RegisterAction(
		const FString& Namespace,
		const FString& Action,
		const FString& Description,
		const FMonolithActionHandler& Handler,
		const TSharedPtr<FJsonObject>& ParamSchema = nullptr,
		const FString& Category = FString(),  // Optional sub-group within namespace — defaults to uncategorized
		const FMonolithActionExecutionPolicy& ExecutionPolicy = FMonolithActionExecutionPolicy::DefaultReadOnly()
	);

	/** Unregister all actions in a namespace (called during module shutdown) */
	void UnregisterNamespace(const FString& Namespace);

	/** Execute an action by namespace + action name */
	FMonolithActionResult ExecuteAction(const FString& Namespace, const FString& Action, const TSharedPtr<FJsonObject>& Params);

	/** Get all registered namespaces */
	TArray<FString> GetNamespaces() const;

	/** Get all actions in a namespace */
	TArray<FMonolithActionInfo> GetActions(const FString& Namespace) const;

	/** Get only the action names for a namespace without deep copying action info */
	TArray<FString> GetActionNames(const FString& Namespace) const;

	/** Get all actions across all namespaces */
	TArray<FMonolithActionInfo> GetAllActions() const;

	/** Check if a specific action exists */
	bool HasAction(const FString& Namespace, const FString& Action) const;

	/** Check if a namespace exists in the raw registry */
	bool HasNamespace(const FString& Namespace) const;

	/** Get action execution policy metadata, or the default read-only policy if missing. */
	FMonolithActionExecutionPolicy GetActionExecutionPolicy(const FString& Namespace, const FString& Action) const;

	/** Override action execution policy metadata for a known action in this process. */
	bool SetActionExecutionPolicy(const FString& Namespace, const FString& Action, const FMonolithActionExecutionPolicy& ExecutionPolicy, FString& OutError);

	/** Get total number of registered actions */
	int32 GetActionCount() const;

	/** Get total number of registered namespaces without allocating a copy */
	int32 GetNamespaceCount() const;

	/** Get number of registered actions in a namespace without allocating a copy */
	int32 GetNamespaceActionCount(const FString& Namespace) const;

	/**
	 * CC-05: Find action names in a namespace that are similar to the given name.
	 * Uses prefix match + Levenshtein distance to surface "did you mean" suggestions.
	 * Returns up to MaxResults names, ordered by best match first.
	 */
	TArray<FString> FindSimilarActions(const FString& Namespace, const FString& ActionName, int32 MaxResults = 5) const;

private:
	FMonolithToolRegistry() = default;

	struct FRegisteredAction
	{
		FMonolithActionInfo Info;
		FMonolithActionHandler Handler;
	};

	/** Map of "namespace.action" → registered action */
	TMap<FString, FRegisteredAction> Actions;

	/** Map of namespace → list of action keys */
	TMap<FString, TArray<FString>> NamespaceActions;

	static FString MakeKey(const FString& Namespace, const FString& Action)
	{
		return Namespace + TEXT(".") + Action;
	}

	mutable FCriticalSection RegistryLock;
};
