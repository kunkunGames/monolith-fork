#pragma once

#include "CoreMinimal.h"
#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"

/**
 * Typed media content block for an MCP tool result (image/audio).
 * Mirrors the MCP content-block shape: {type, mimeType, data(base64)}.
 * Audience is optional MCP annotation metadata ("user"/"assistant"); empty = omitted.
 */
struct FMonolithToolContentBlock
{
	FString Type;        // "image" or "audio"
	FString MimeType;    // e.g. "image/png", "audio/wav"
	FString Base64Data;  // base64-encoded payload
	FString Audience;    // optional MCP annotation; empty = omit
};

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

	// Typed-media slot — empty by default → existing responses byte-identical.
	// Populated only by handlers that opt into image/audio content blocks; emission
	// is additionally gated by UMonolithSettings::bEnableTypedMediaResults at the call site.
	TArray<FMonolithToolContentBlock> MediaBlocks;

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

/**
 * Optional metadata used by monolith.find ranking and unknown-action recovery.
 * Aliases are search-only names: they do not register dispatch endpoints, and
 * recovery returns the current registered action name rather than the alias.
 */
struct MONOLITHCORE_API FMonolithActionSearchMetadata
{
	TArray<FString> Keywords;
	TArray<FString> Aliases;
	TArray<FString> Examples;

	bool IsEmpty() const
	{
		return Keywords.Num() == 0 && Aliases.Num() == 0 && Examples.Num() == 0;
	}
};

/** Planning metadata emitted by discovery so agents can choose skills and compose follow-up calls. */
struct MONOLITHCORE_API FMonolithActionPlanningMetadata
{
	FString Skill;
	TArray<FString> Preconditions;
	TArray<FString> Outputs;
	TArray<FString> NextActions;

	bool IsEmpty() const
	{
		return Skill.IsEmpty()
			&& Preconditions.Num() == 0
			&& Outputs.Num() == 0
			&& NextActions.Num() == 0;
	}
};

/** Metadata describing a registered action */
struct FMonolithActionInfo
{
	FString Namespace;
	FString Action;
	FString Description;
	FString Category;                     // Optional sub-grouping within a namespace (e.g. "CommonUI" inside "ui"). Empty = uncategorized.
	FMonolithActionExecutionPolicy ExecutionPolicy;
	FMonolithActionSearchMetadata SearchMetadata;
	FMonolithActionPlanningMetadata PlanningMetadata;
	TSharedPtr<FJsonObject> ParamSchema;  // JSON Schema for parameter validation

	// Survivor A (plan §3.A) — MCP-spec tool annotation hints. Only emitted on
	// `tools/list` when at least one is non-default; per-call runtime cost is zero.
	// For individually-registered tools (`monolith_*`) the hints are read from the
	// action's own info. For namespace dispatcher tools (`*_query`) the hints come
	// from FMonolithToolRegistry::GetDispatcherAnnotations() — see that path for
	// the rationale (sibling actions in the same dispatcher can disagree on
	// destructive/read-only, so the dispatcher-level annotation is authoritative).
	bool bReadOnlyHint   = false;
	bool bDestructiveHint = false;
	bool bIdempotentHint = false;
	FString Title;
};

/**
 * Survivor A (plan §3.A) — per-namespace dispatcher annotations.
 * Used for namespace dispatcher tools (`source_query` etc.) where the four MCP
 * hint fields apply to the WHOLE dispatcher rather than any single action. Held
 * separately from FMonolithActionInfo because the dispatcher is not a
 * registered action — it is synthesised inside HandleToolsList at serialize time.
 */
struct FMonolithDispatcherAnnotations
{
	bool bReadOnlyHint   = false;
	bool bDestructiveHint = false;
	bool bIdempotentHint = false;
	FString Title;

	/** Helper: true iff any hint is non-default. Drives "do we emit annotations on the wire?" */
	bool IsAnyNonDefault() const
	{
		return bReadOnlyHint || bDestructiveHint || bIdempotentHint || !Title.IsEmpty();
	}
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
		const FMonolithActionExecutionPolicy& ExecutionPolicy = FMonolithActionExecutionPolicy::DefaultReadOnly(),
		const FMonolithActionSearchMetadata& SearchMetadata = FMonolithActionSearchMetadata(),
		const FMonolithActionPlanningMetadata& PlanningMetadata = FMonolithActionPlanningMetadata()
	);

	/**
	 * Attach search metadata (keywords/aliases/examples) to an already-registered action
	 * for monolith.find ranking and unknown-action recovery. Registry-owned compatibility
	 * aliases are reapplied after these arrays are replaced. Safe post-registration setter:
	 * it does NOT touch execution policy, unlike passing SearchMetadata as the 8th
	 * RegisterAction positional arg (which would force re-specifying Category/ExecutionPolicy
	 * and could downgrade a write action's transaction policy). Idempotent; call after the
	 * action's RegisterAction (typically at the end of a module's RegisterAll).
	 * @return true if the action existed and was updated; false (with a warning) otherwise.
	 */
	bool SetActionSearchMetadata(
		const FString& Namespace,
		const FString& Action,
		const TArray<FString>& Keywords,
		const TArray<FString>& Aliases = TArray<FString>(),
		const TArray<FString>& Examples = TArray<FString>());

	/**
	 * Attach planning metadata to an already-registered action. Discovery emits
	 * derived defaults for every action; this setter adds domain-specific
	 * preconditions, outputs, next action hints, or a skill override.
	 */
	bool SetActionPlanningMetadata(
		const FString& Namespace,
		const FString& Action,
		const FString& Skill,
		const TArray<FString>& Preconditions = TArray<FString>(),
		const TArray<FString>& Outputs = TArray<FString>(),
		const TArray<FString>& NextActions = TArray<FString>());

	/**
	 * Register a batch of actions as owned by a module or action bundle.
	 * Owner tags allow module shutdown to remove only its own actions from shared namespaces.
	 */
	void RegisterOwnedActions(const FString& Owner, TFunctionRef<void(FMonolithToolRegistry&)> Register);

	/** Unregister one action. Returns true when an action was removed. */
	bool UnregisterAction(const FString& Namespace, const FString& Action);

	/** Unregister all actions owned by Owner. Returns the number of removed actions. */
	int32 UnregisterOwner(const FString& Owner);

	/** Unregister all actions in a namespace (called during module shutdown) */
	void UnregisterNamespace(const FString& Namespace);

	/** Execute an action by namespace + action name */
	FMonolithActionResult ExecuteAction(const FString& Namespace, const FString& Action, const TSharedPtr<FJsonObject>& Params);

	/** Resolve the agent skill that owns workflow guidance for a namespace. */
	static FString ResolveSkillForNamespace(const FString& Namespace);

	/** Build factual, registry-derived planning signals for discovery rows and failure diagnostics. */
	static TArray<TSharedPtr<FJsonValue>> BuildPlanningSignals(const FMonolithActionInfo& ActionInfo);

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
	 * Stable content fingerprint ("sha256:<16 hex>") of the profile-visible catalog:
	 * sorted action keys, descriptions, categories, execution policy ids, param
	 * schemas, search/planning metadata, MCP annotations, and the active profile id.
	 * Changes whenever discovery output could change; drives the
	 * monolith.status/discover `catalog_version` + `if_version` short-circuit.
	 * Computed on demand without a cache: catalog mutations happen at module
	 * startup/shutdown while status/discover calls are low-frequency.
	 */
	FString GetCatalogFingerprint() const;

	/**
	 * CC-05: Find action names in a namespace that are similar to the given name.
	 * Profile-filtered registered names and aliases use exact, prefix, substring,
	 * then bounded-Levenshtein scoring. Returns only registered action names, up
	 * to MaxResults, with deterministic best-match-first ordering.
	 */
	TArray<FString> FindSimilarActions(const FString& Namespace, const FString& ActionName, int32 MaxResults = 5) const;

	/**
	 * Survivor A (plan §3.A) — Set MCP hint annotations for a namespace dispatcher
	 * tool (e.g. `source_query`). These are serialized into `tools/list` under the
	 * dispatcher tool's `annotations` sub-object. Only namespaces whose dispatcher
	 * is audited as safe (read-only / idempotent) should call this. Defaults are
	 * preserved when no call is made — so untagged dispatchers stay defaulted.
	 *
	 * Thread-safe: takes RegistryLock internally.
	 */
	void SetDispatcherAnnotations(const FString& Namespace, const FMonolithDispatcherAnnotations& Annotations);

	/**
	 * Survivor A — Look up dispatcher annotations for a namespace. Returns a
	 * default-constructed (all-false / empty-title) struct if the namespace was
	 * never annotated. Used by HandleToolsList to decide whether to emit the
	 * MCP `annotations` block.
	 */
	FMonolithDispatcherAnnotations GetDispatcherAnnotations(const FString& Namespace) const;

	/**
	 * Survivor A (plan §3.A) — Set MCP hint annotations on an already-registered
	 * action. Used for individually-registered top-level tools (`monolith_discover`,
	 * `monolith_status`, etc.). No-op if the action is not registered (safe to
	 * call defensively at module init order boundaries).
	 *
	 * Thread-safe: takes RegistryLock internally.
	 */
	void SetActionAnnotations(
		const FString& Namespace,
		const FString& Action,
		bool bReadOnly,
		bool bDestructive,
		bool bIdempotent,
		const FString& Title);

private:
	FMonolithToolRegistry() = default;

	struct FRegisteredAction
	{
		FMonolithActionInfo Info;
		FMonolithActionHandler Handler;
		FString Owner;
	};

	/** Map of "namespace.action" → registered action */
	TMap<FString, FRegisteredAction> Actions;

	/** Map of namespace → list of action keys */
	TMap<FString, TArray<FString>> NamespaceActions;

	/** Stack of module/action-bundle owners active during registration. */
	TArray<FString> RegistrationOwnerStack;

	bool UnregisterActionByKey_NoLock(const FString& Key);

	/** Survivor A — Map of namespace → dispatcher-level MCP hint annotations. */
	TMap<FString, FMonolithDispatcherAnnotations> DispatcherAnnotations;

	static FString MakeKey(const FString& Namespace, const FString& Action)
	{
		return Namespace + TEXT(".") + Action;
	}

	static FMonolithActionExecutionPolicy InferExecutionPolicy(const FString& Namespace, const FString& Action, const FMonolithActionExecutionPolicy& RequestedPolicy);

	mutable FCriticalSection RegistryLock;
};
