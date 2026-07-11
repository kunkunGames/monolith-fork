// Copyright tumourlove. All Rights Reserved.

#include "Actions/MonolithUIContextActions.h"

#include "Animation/WidgetAnimation.h"
#include "Blueprint/WidgetTree.h"
#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "HAL/CriticalSection.h"
#include "Misc/ScopeLock.h"
#include "MonolithExecutionContext.h"
#include "MonolithParamSchema.h"
#include "MonolithToolInvocationLogger.h"
#include "MonolithToolRegistry.h"
#include "MonolithUICommon.h"
#include "WidgetBlueprint.h"

namespace MonolithUI::ContextActionsInternal
{
	static constexpr int32 DefaultTtlSeconds = 900;
	static constexpr int32 MaxRecentContexts = 10;

	struct FWidgetContextEntry
	{
		FString AssetPath;
		FString WidgetName;
		FString AnimationName;
		FString Scope;
		FString Source;
		FString SessionKey;
		FString RequestKey;
		FDateTime CreatedUtc;
		FDateTime UpdatedUtc;
		FDateTime ExpiresUtc;
		int32 TtlSeconds = DefaultTtlSeconds;

		bool IsExpired(const FDateTime& NowUtc) const
		{
			return ExpiresUtc != FDateTime() && ExpiresUtc <= NowUtc;
		}
	};

	struct FContextBucket
	{
		TOptional<FWidgetContextEntry> SessionContext;
		TMap<FString, FWidgetContextEntry> RequestContexts;
		TArray<FWidgetContextEntry> Recent;
	};

	FCriticalSection GContextMutex;
	TMap<FString, FContextBucket> GBucketsBySession;

	static FString NormalizeScope(FString Scope, const FString& DefaultScope = TEXT("session"))
	{
		Scope = Scope.TrimStartAndEnd().ToLower();
		if (Scope == TEXT("request") || Scope == TEXT("session") || Scope == TEXT("auto") || Scope == TEXT("all"))
		{
			return Scope;
		}
		return DefaultScope;
	}

	static FString CurrentSessionKey()
	{
		const FString TraceSessionKey = FMonolithToolInvocationLogger::GetCurrentSessionKey();
		if (!TraceSessionKey.IsEmpty())
		{
			return TraceSessionKey;
		}

		if (const FMonolithExecutionContext* Context = FMonolithExecutionContext::GetCurrent())
		{
			const FString& Session = Context->GetSessionIdRedacted();
			return Session.IsEmpty() ? FString(TEXT("stateless")) : Session;
		}
		return TEXT("stateless");
	}

	static FString CurrentRequestKey()
	{
		if (const FMonolithExecutionContext* Context = FMonolithExecutionContext::GetCurrent())
		{
			const FString& ToolCallId = Context->GetToolCallId();
			if (!ToolCallId.IsEmpty())
			{
				return ToolCallId;
			}
			return Context->GetJsonRpcId();
		}
		return TEXT("stateless-request");
	}

	static TSharedPtr<FJsonObject> MakeNextAction(const TCHAR* Tool, const bool bAvailable, const TCHAR* Purpose)
	{
		TSharedPtr<FJsonObject> Obj = MakeShared<FJsonObject>();
		Obj->SetStringField(TEXT("tool"), Tool);
		Obj->SetBoolField(TEXT("available"), bAvailable);
		Obj->SetStringField(TEXT("purpose"), Purpose);
		return Obj;
	}

	static UWidgetAnimation* FindAnimationByName(UWidgetBlueprint* WBP, const FString& AnimationName)
	{
		if (!WBP || AnimationName.IsEmpty())
		{
			return nullptr;
		}

		const FName TargetName(*AnimationName);
		for (UWidgetAnimation* Animation : WBP->Animations)
		{
			if (Animation && (Animation->GetFName() == TargetName || Animation->GetName() == AnimationName))
			{
				return Animation;
			}
		}
		return nullptr;
	}

	static TSharedPtr<FJsonObject> ResolveContext(const FWidgetContextEntry& Entry)
	{
		TSharedPtr<FJsonObject> Resolved = MakeShared<FJsonObject>();
		Resolved->SetBoolField(TEXT("asset_checked"), !Entry.AssetPath.IsEmpty());
		Resolved->SetBoolField(TEXT("widget_checked"), !Entry.WidgetName.IsEmpty());
		Resolved->SetBoolField(TEXT("animation_checked"), !Entry.AnimationName.IsEmpty());
		Resolved->SetBoolField(TEXT("asset_exists"), false);
		Resolved->SetBoolField(TEXT("widget_exists"), false);
		Resolved->SetBoolField(TEXT("animation_exists"), false);

		if (Entry.AssetPath.IsEmpty())
		{
			Resolved->SetStringField(TEXT("status"), TEXT("no_asset_path"));
			return Resolved;
		}

		FMonolithActionResult LoadError;
		UWidgetBlueprint* WBP = MonolithUI::LoadWidgetBlueprint(Entry.AssetPath, LoadError);
		if (!WBP)
		{
			Resolved->SetStringField(TEXT("status"), TEXT("stale_asset_missing"));
			if (!LoadError.ErrorMessage.IsEmpty())
			{
				Resolved->SetStringField(TEXT("asset_error"), LoadError.ErrorMessage);
			}
			return Resolved;
		}

		Resolved->SetBoolField(TEXT("asset_exists"), true);
		Resolved->SetStringField(TEXT("asset_class"), WBP->GetClass()->GetPathName());

		if (!Entry.WidgetName.IsEmpty())
		{
			const bool bWidgetExists = WBP->WidgetTree
				&& WBP->WidgetTree->FindWidget(FName(*Entry.WidgetName)) != nullptr;
			Resolved->SetBoolField(TEXT("widget_exists"), bWidgetExists);
		}
		else
		{
			Resolved->SetBoolField(TEXT("widget_exists"), true);
		}

		if (!Entry.AnimationName.IsEmpty())
		{
			Resolved->SetBoolField(TEXT("animation_exists"), FindAnimationByName(WBP, Entry.AnimationName) != nullptr);
		}
		else
		{
			Resolved->SetBoolField(TEXT("animation_exists"), true);
		}

		bool bWidgetExists = false;
		bool bHasWidgetField = Resolved->TryGetBoolField(TEXT("widget_exists"), bWidgetExists);
		const bool bStaleWidget = !Entry.WidgetName.IsEmpty() && bHasWidgetField && !bWidgetExists;
		bool bAnimationExists = false;
		bool bHasAnimationField = Resolved->TryGetBoolField(TEXT("animation_exists"), bAnimationExists);
		const bool bStaleAnimation = !Entry.AnimationName.IsEmpty() && bHasAnimationField && !bAnimationExists;
		Resolved->SetStringField(TEXT("status"), bStaleWidget || bStaleAnimation ? TEXT("stale_reference") : TEXT("fresh"));
		return Resolved;
	}

	static TSharedPtr<FJsonObject> EntryToJson(
		const FWidgetContextEntry& Entry,
		const FString& ResolvedFrom,
		const FDateTime& NowUtc,
		const bool bIncludeResolved)
	{
		TSharedPtr<FJsonObject> Context = MakeShared<FJsonObject>();
		Context->SetStringField(TEXT("asset_path"), Entry.AssetPath);
		Context->SetStringField(TEXT("widget_name"), Entry.WidgetName);
		Context->SetStringField(TEXT("animation_name"), Entry.AnimationName);
		Context->SetStringField(TEXT("scope"), Entry.Scope);
		Context->SetStringField(TEXT("source"), Entry.Source);
		Context->SetStringField(TEXT("session_key"), Entry.SessionKey);
		Context->SetStringField(TEXT("request_key"), Entry.RequestKey);
		Context->SetStringField(TEXT("created_at"), Entry.CreatedUtc.ToIso8601());
		Context->SetStringField(TEXT("updated_at"), Entry.UpdatedUtc.ToIso8601());
		Context->SetStringField(TEXT("expires_at"), Entry.ExpiresUtc == FDateTime() ? FString() : Entry.ExpiresUtc.ToIso8601());
		Context->SetNumberField(TEXT("ttl_seconds"), Entry.TtlSeconds);

		TSharedPtr<FJsonObject> Obj = MakeShared<FJsonObject>();
		Obj->SetBoolField(TEXT("ok"), true);
		Obj->SetStringField(TEXT("schema_version"), TEXT("ui_widget_context.v1"));
		Obj->SetBoolField(TEXT("has_context"), true);
		Obj->SetStringField(TEXT("resolved_from"), ResolvedFrom);
		Obj->SetStringField(TEXT("freshness"), Entry.IsExpired(NowUtc) ? TEXT("expired") : TEXT("fresh"));
		Obj->SetBoolField(TEXT("expired"), Entry.IsExpired(NowUtc));
		Obj->SetObjectField(TEXT("context"), Context);
		Obj->SetBoolField(TEXT("usable_as_default_for_mutation"), false);
		Obj->SetStringField(TEXT("mutation_contract"), TEXT("All mutating ui actions still require explicit asset_path/widget/action parameters."));
		if (bIncludeResolved)
		{
			Obj->SetObjectField(TEXT("resolved"), ResolveContext(Entry));
		}
		return Obj;
	}

	static void AddRecent(FContextBucket& Bucket, const FWidgetContextEntry& Entry)
	{
		Bucket.Recent.RemoveAll([&Entry](const FWidgetContextEntry& Existing)
		{
			return Existing.AssetPath == Entry.AssetPath
				&& Existing.WidgetName == Entry.WidgetName
				&& Existing.AnimationName == Entry.AnimationName
				&& Existing.Scope == Entry.Scope;
		});
		Bucket.Recent.Insert(Entry, 0);
		if (Bucket.Recent.Num() > MaxRecentContexts)
		{
			Bucket.Recent.SetNum(MaxRecentContexts);
		}
	}

	static void AddRecentArray(
		TSharedPtr<FJsonObject> Result,
		const TArray<FWidgetContextEntry>& Recent,
		const FDateTime& NowUtc,
		const int32 Limit)
	{
		TArray<TSharedPtr<FJsonValue>> Values;
		const int32 ClampedLimit = FMath::Clamp(Limit, 0, MaxRecentContexts);
		Values.Reserve(FMath::Min(Recent.Num(), ClampedLimit));
		for (int32 Index = 0; Index < Recent.Num() && Values.Num() < ClampedLimit; ++Index)
		{
			const FWidgetContextEntry& Entry = Recent[Index];
			TSharedPtr<FJsonObject> Row = MakeShared<FJsonObject>();
			Row->SetStringField(TEXT("asset_path"), Entry.AssetPath);
			Row->SetStringField(TEXT("widget_name"), Entry.WidgetName);
			Row->SetStringField(TEXT("animation_name"), Entry.AnimationName);
			Row->SetStringField(TEXT("scope"), Entry.Scope);
			Row->SetStringField(TEXT("updated_at"), Entry.UpdatedUtc.ToIso8601());
			Row->SetBoolField(TEXT("expired"), Entry.IsExpired(NowUtc));
			Values.Add(MakeShared<FJsonValueObject>(Row));
		}
		Result->SetArrayField(TEXT("recent_contexts"), Values);
		Result->SetNumberField(TEXT("recent_context_count"), Values.Num());
	}

	static FMonolithActionResult HandleSetWidgetContext(const TSharedPtr<FJsonObject>& Params)
	{
		if (!Params.IsValid())
		{
			return FMonolithActionResult::Error(TEXT("Missing params object"), -32602);
		}

		FString AssetPath;
		Params->TryGetStringField(TEXT("asset_path"), AssetPath);
		FString WidgetName;
		Params->TryGetStringField(TEXT("widget_name"), WidgetName);
		FString AnimationName;
		Params->TryGetStringField(TEXT("animation_name"), AnimationName);
		FString ScopeParam;
		Params->TryGetStringField(TEXT("scope"), ScopeParam);
		const FString Scope = NormalizeScope(ScopeParam, TEXT("session"));
		if (Scope != TEXT("session") && Scope != TEXT("request"))
		{
			return FMonolithActionResult::Error(TEXT("scope must be 'session' or 'request' for set_widget_context"), -32602);
		}

		int32 TtlSeconds = DefaultTtlSeconds;
		double TtlNumber = 0.0;
		if (Params->TryGetNumberField(TEXT("ttl_seconds"), TtlNumber))
		{
			TtlSeconds = FMath::Clamp(FMath::RoundToInt(TtlNumber), 1, 24 * 60 * 60);
		}

		const FDateTime NowUtc = FDateTime::UtcNow();
		FWidgetContextEntry Entry;
		Entry.AssetPath = AssetPath.TrimStartAndEnd();
		Entry.WidgetName = WidgetName.TrimStartAndEnd();
		Entry.AnimationName = AnimationName.TrimStartAndEnd();
		Entry.Scope = Scope;
		Entry.Source = TEXT("explicit_set");
		Entry.SessionKey = CurrentSessionKey();
		Entry.RequestKey = CurrentRequestKey();
		Entry.CreatedUtc = NowUtc;
		Entry.UpdatedUtc = NowUtc;
		Entry.TtlSeconds = TtlSeconds;
		Entry.ExpiresUtc = NowUtc + FTimespan::FromSeconds(TtlSeconds);

		{
			FScopeLock Lock(&GContextMutex);
			FContextBucket& Bucket = GBucketsBySession.FindOrAdd(Entry.SessionKey);
			if (Scope == TEXT("request"))
			{
				Bucket.RequestContexts.Add(Entry.RequestKey, Entry);
			}
			else
			{
				Bucket.SessionContext = Entry;
			}
			AddRecent(Bucket, Entry);
		}

		TSharedPtr<FJsonObject> Result = EntryToJson(
			Entry,
			Scope == TEXT("request") ? TEXT("request_context") : TEXT("session_context"),
			NowUtc,
			/*bIncludeResolved=*/true);
		Result->SetBoolField(TEXT("changed"), true);
		return FMonolithActionResult::Success(Result);
	}

	static bool TrySelectContext(
		const FContextBucket& Bucket,
		const FString& Scope,
		const FString& RequestKey,
		const FDateTime& NowUtc,
		FWidgetContextEntry& OutEntry,
		FString& OutResolvedFrom)
	{
		if ((Scope == TEXT("auto") || Scope == TEXT("request")))
		{
			if (const FWidgetContextEntry* RequestEntry = Bucket.RequestContexts.Find(RequestKey))
			{
				if (!RequestEntry->IsExpired(NowUtc))
				{
					OutEntry = *RequestEntry;
					OutResolvedFrom = TEXT("request_context");
					return true;
				}
				if (Scope == TEXT("request"))
				{
					OutEntry = *RequestEntry;
					OutResolvedFrom = TEXT("expired_request_context");
					return true;
				}
			}
		}

		if ((Scope == TEXT("auto") || Scope == TEXT("session")) && Bucket.SessionContext.IsSet())
		{
			OutEntry = Bucket.SessionContext.GetValue();
			OutResolvedFrom = OutEntry.IsExpired(NowUtc) ? TEXT("expired_session_context") : TEXT("session_context");
			return true;
		}
		return false;
	}

	static FMonolithActionResult HandleGetWidgetContext(const TSharedPtr<FJsonObject>& Params)
	{
		FString ScopeParam;
		if (Params.IsValid())
		{
			Params->TryGetStringField(TEXT("scope"), ScopeParam);
		}
		const FString Scope = NormalizeScope(ScopeParam, TEXT("auto"));
		if (Scope != TEXT("auto") && Scope != TEXT("session") && Scope != TEXT("request"))
		{
			return FMonolithActionResult::Error(TEXT("scope must be 'auto', 'session', or 'request' for get_widget_context"), -32602);
		}

		int32 RecentLimit = 5;
		bool bIncludeRecent = true;
		if (Params.IsValid())
		{
			Params->TryGetBoolField(TEXT("include_recent"), bIncludeRecent);
			double LimitNumber = 0.0;
			if (Params->TryGetNumberField(TEXT("recent_limit"), LimitNumber))
			{
				RecentLimit = FMath::Clamp(FMath::RoundToInt(LimitNumber), 0, MaxRecentContexts);
			}
		}

		const FString SessionKey = CurrentSessionKey();
		const FString RequestKey = CurrentRequestKey();
		const FDateTime NowUtc = FDateTime::UtcNow();
		TOptional<FWidgetContextEntry> Selected;
		FString ResolvedFrom = TEXT("none");
		TArray<FWidgetContextEntry> RecentCopy;

		{
			FScopeLock Lock(&GContextMutex);
			if (const FContextBucket* Bucket = GBucketsBySession.Find(SessionKey))
			{
				FWidgetContextEntry Entry;
				if (TrySelectContext(*Bucket, Scope, RequestKey, NowUtc, Entry, ResolvedFrom))
				{
					Selected = Entry;
				}
				RecentCopy = Bucket->Recent;
			}
		}

		TSharedPtr<FJsonObject> Result;
		if (Selected.IsSet())
		{
			Result = EntryToJson(Selected.GetValue(), ResolvedFrom, NowUtc, /*bIncludeResolved=*/true);
		}
		else
		{
			Result = MakeShared<FJsonObject>();
			Result->SetBoolField(TEXT("ok"), true);
			Result->SetStringField(TEXT("schema_version"), TEXT("ui_widget_context.v1"));
			Result->SetBoolField(TEXT("has_context"), false);
			Result->SetStringField(TEXT("resolved_from"), TEXT("none"));
			Result->SetStringField(TEXT("freshness"), TEXT("no_context"));
			Result->SetBoolField(TEXT("usable_as_default_for_mutation"), false);
			Result->SetStringField(TEXT("mutation_contract"), TEXT("All mutating ui actions still require explicit asset_path/widget/action parameters."));
		}

		Result->SetStringField(TEXT("session_key"), SessionKey);
		Result->SetStringField(TEXT("request_key"), RequestKey);
		if (bIncludeRecent)
		{
			AddRecentArray(Result, RecentCopy, NowUtc, RecentLimit);
		}

		TArray<TSharedPtr<FJsonValue>> NextActions;
		NextActions.Add(MakeShared<FJsonValueObject>(MakeNextAction(
			TEXT("ui.get_widget_tree"),
			FMonolithToolRegistry::Get().HasAction(TEXT("ui"), TEXT("get_widget_tree")),
			TEXT("Inspect the explicit target widget tree before writing."))));
		NextActions.Add(MakeShared<FJsonValueObject>(MakeNextAction(
			TEXT("ui.describe_widget_type_schema"),
			FMonolithToolRegistry::Get().HasAction(TEXT("ui"), TEXT("describe_widget_type_schema")),
			TEXT("Discover safe properties for the explicit target widget."))));
		Result->SetArrayField(TEXT("next_actions"), NextActions);
		return FMonolithActionResult::Success(Result);
	}

	static FMonolithActionResult HandleClearWidgetContext(const TSharedPtr<FJsonObject>& Params)
	{
		FString ScopeParam;
		if (Params.IsValid())
		{
			Params->TryGetStringField(TEXT("scope"), ScopeParam);
		}
		const FString Scope = NormalizeScope(ScopeParam, TEXT("all"));
		if (Scope != TEXT("all") && Scope != TEXT("session") && Scope != TEXT("request"))
		{
			return FMonolithActionResult::Error(TEXT("scope must be 'all', 'session', or 'request' for clear_widget_context"), -32602);
		}

		const FString SessionKey = CurrentSessionKey();
		const FString RequestKey = CurrentRequestKey();
		int32 ClearedCount = 0;

		{
			FScopeLock Lock(&GContextMutex);
			if (FContextBucket* Bucket = GBucketsBySession.Find(SessionKey))
			{
				if ((Scope == TEXT("all") || Scope == TEXT("session")) && Bucket->SessionContext.IsSet())
				{
					Bucket->SessionContext.Reset();
					++ClearedCount;
				}
				if (Scope == TEXT("all"))
				{
					ClearedCount += Bucket->RequestContexts.Num();
					Bucket->RequestContexts.Empty();
				}
				else if (Scope == TEXT("request"))
				{
					ClearedCount += Bucket->RequestContexts.Remove(RequestKey);
				}
				if (Scope == TEXT("all"))
				{
					Bucket->Recent.Empty();
				}
			}
		}

		TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
		Result->SetBoolField(TEXT("ok"), true);
		Result->SetStringField(TEXT("schema_version"), TEXT("ui_widget_context.v1"));
		Result->SetStringField(TEXT("scope"), Scope);
		Result->SetStringField(TEXT("session_key"), SessionKey);
		Result->SetStringField(TEXT("request_key"), RequestKey);
		Result->SetNumberField(TEXT("cleared_count"), ClearedCount);
		Result->SetBoolField(TEXT("usable_as_default_for_mutation"), false);
		Result->SetStringField(TEXT("mutation_contract"), TEXT("All mutating ui actions still require explicit asset_path/widget/action parameters."));
		return FMonolithActionResult::Success(Result);
	}
}

void MonolithUI::FContextActions::Register(FMonolithToolRegistry& Registry)
{
	using namespace ContextActionsInternal;

	Registry.RegisterAction(
		TEXT("ui"), TEXT("set_widget_context"),
		TEXT("Set explicit UMG work context for the current MCP session/request. This never auto-creates assets and never becomes a hidden fallback for mutating UI actions."),
		FMonolithActionHandler::CreateStatic(&HandleSetWidgetContext),
		FParamSchemaBuilder()
			.OptionalAssetPath(TEXT("asset_path"), TEXT("Widget Blueprint asset path to remember as context."))
			.Optional(TEXT("widget_name"), TEXT("string"), TEXT("Widget name inside asset_path to remember."))
			.Optional(TEXT("animation_name"), TEXT("string"), TEXT("Widget animation name inside asset_path to remember."))
			.Optional(TEXT("scope"), TEXT("string"), TEXT("Context scope: session or request. Request scope is visible only to the current tool-call context."), TEXT("session"))
			.Optional(TEXT("ttl_seconds"), TEXT("integer"), TEXT("Context TTL in seconds, clamped to 1..86400."), TEXT("900"))
			.Build(),
		TEXT("Context")
	);

	Registry.RegisterAction(
		TEXT("ui"), TEXT("get_widget_context"),
		TEXT("Read explicit UMG work context for the current MCP session/request, including freshness and resolved asset/widget/animation existence. Diagnostic only; mutating actions still require explicit targets."),
		FMonolithActionHandler::CreateStatic(&HandleGetWidgetContext),
		FParamSchemaBuilder()
			.Optional(TEXT("scope"), TEXT("string"), TEXT("Context scope to read: auto, session, or request."), TEXT("auto"))
			.Optional(TEXT("include_recent"), TEXT("boolean"), TEXT("Include recent contexts for this session."), TEXT("true"))
			.Optional(TEXT("recent_limit"), TEXT("integer"), TEXT("Maximum recent context rows to return."), TEXT("5"))
			.Build(),
		TEXT("Context")
	);

	Registry.RegisterAction(
		TEXT("ui"), TEXT("clear_widget_context"),
		TEXT("Clear explicit UMG work context for the current MCP session/request. Does not affect other sessions and does not mutate any UMG asset."),
		FMonolithActionHandler::CreateStatic(&HandleClearWidgetContext),
		FParamSchemaBuilder()
			.Optional(TEXT("scope"), TEXT("string"), TEXT("Scope to clear: all, session, or request."), TEXT("all"))
			.Build(),
		TEXT("Context")
	);

	FMonolithToolRegistry::Get().SetActionSearchMetadata(TEXT("ui"), TEXT("set_widget_context"),
		{ TEXT("UMG target context"), TEXT("remember widget asset"), TEXT("focus widget blueprint"), TEXT("ui work context") },
		{ TEXT("set_target_umg_asset"), TEXT("set_target_widget"), TEXT("widget_target"), TEXT("animation_target") },
		{ TEXT("remember WBP_Menu StartButton as the current UI context"), TEXT("set widget context for this session without mutating the WBP") });

	FMonolithToolRegistry::Get().SetActionSearchMetadata(TEXT("ui"), TEXT("get_widget_context"),
		{ TEXT("current UMG target"), TEXT("last edited widget asset"), TEXT("recent widget context"), TEXT("ui work context") },
		{ TEXT("get_target_umg_asset"), TEXT("get_target_widget"), TEXT("get_last_edited_umg_asset"), TEXT("get_recently_edited_umg_assets") },
		{ TEXT("show current UI context and whether the widget still exists"), TEXT("list recent UMG contexts for this session") });

	FMonolithToolRegistry::Get().SetActionSearchMetadata(TEXT("ui"), TEXT("clear_widget_context"),
		{ TEXT("clear UMG context"), TEXT("reset widget focus"), TEXT("forget widget target"), TEXT("ui work context") },
		{ TEXT("clear_target_umg_asset"), TEXT("reset_widget_context") },
		{ TEXT("clear the current UI context for this session") });
}
