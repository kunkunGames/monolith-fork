// SPDX-License-Identifier: MIT
// Central dispatcher actions for the bulk_fill + describe namespaces.
// Routes incoming JSON-RPC params to the per-namespace adapter table.
// Phase 0 — registered from FMonolithCoreModule::StartupModule.

#include "Actions/MonolithBulkFillActions.h"
#include "MonolithBulkFillRegistry.h"
#include "MonolithBulkFillTypes.h"
#include "Reflection/MonolithDryRunGuard.h"
#include "MonolithToolRegistry.h"
#include "MonolithJsonUtils.h"
#include "MonolithParamSchema.h"
#include "Dom/JsonObject.h"

#include <initializer_list>

namespace MonolithBulkFillActionsInternal
{
	static TSharedPtr<FJsonObject> BuildBulkFillSchema()
	{
		return FParamSchemaBuilder()
			.Required(TEXT("target_namespace"), TEXT("string"),
				TEXT("Adapter namespace ('blueprint', 'gas', 'inventory', 'ui', 'ai', 'niagara', 'material', 'audio', 'mesh', 'animation', 'logicdriver', 'combograph'). Alias: `namespace`."),
				{ TEXT("namespace") })
			.Required(TEXT("target"), TEXT("string"),
				TEXT("Asset path or adapter-defined target identifier (e.g. '/Game/Items/DA_HealingPotion')."))
			.Required(TEXT("tree"), TEXT("object"),
				TEXT("Nested JSON object of properties to walk against the target's reflection schema."))
			.Optional(TEXT("dry_run"), TEXT("boolean"),
				TEXT("If true, validate only — emit the would-be writes but do not persist."),
				TEXT("false"))
			.Optional(TEXT("strict"), TEXT("boolean"),
				TEXT("If true, promote silent drops / clamps / unknown-fields to hard errors."),
				TEXT("false"))
			.Build();
	}

	static TSharedPtr<FJsonObject> BuildBulkFillListNamespacesSchema()
	{
		return FParamSchemaBuilder().Build();
	}

	static TSharedPtr<FJsonObject> BuildDescribeSchema()
	{
		return FParamSchemaBuilder()
			.Optional(TEXT("target_namespace"), TEXT("string"),
				TEXT("Adapter namespace whose schema should be introspected. Optional so empty/misrouted calls can return registered namespace guidance instead of a validation dead-end. Aliases: `namespace`, `domain`."),
				{ TEXT("namespace"), TEXT("domain") })
			.Optional(TEXT("target"), TEXT("string"),
				TEXT("Asset path or action name to describe. Omit or pass an empty string for the adapter's namespace-level writable-shape summary."))
			.Build();
	}

	static TSharedPtr<FJsonObject> BuildDescribeListTargetsSchema()
	{
		return FParamSchemaBuilder()
			.Optional(TEXT("target_namespace"), TEXT("string"),
				TEXT("Registered adapter namespace whose optional introspection inventory should be listed. Optional so empty/misrouted calls can return registered namespace guidance. If inventory_supported=false, the namespace is registered but does not expose an authoritative target list; use describe.schema without target for the namespace-level shape or with a known target for target-specific shape. Aliases: `namespace`, `domain`."),
				{ TEXT("namespace"), TEXT("domain") })
			.Build();
	}

	// Serialise an FSchemaDescriptor tree to JSON for the response payload.
	static TSharedPtr<FJsonObject> DescriptorToJson(const FSchemaDescriptor& Desc)
	{
		TSharedPtr<FJsonObject> O = MakeShared<FJsonObject>();
		O->SetStringField(TEXT("field_path"), Desc.FieldPath);
		O->SetStringField(TEXT("type_name"), Desc.TypeName);
		O->SetStringField(TEXT("import_text_form"), Desc.ImportTextForm);
		O->SetBoolField(TEXT("required"), Desc.bRequired);
		O->SetBoolField(TEXT("set_once"), Desc.bSetOnce);
		O->SetBoolField(TEXT("pie_blocked"), Desc.bPieBlocked);
		if (!Desc.ConditionalOn.IsEmpty())
		{
			O->SetStringField(TEXT("conditional_on"), Desc.ConditionalOn);
		}
		O->SetNumberField(TEXT("range_min"), Desc.RangeMin);
		O->SetNumberField(TEXT("range_max"), Desc.RangeMax);
		if (Desc.EnumValues.Num() > 0)
		{
			TArray<TSharedPtr<FJsonValue>> Vals;
			for (const FString& E : Desc.EnumValues) { Vals.Add(MakeShared<FJsonValueString>(E)); }
			O->SetArrayField(TEXT("enum_values"), Vals);
		}
		if (Desc.Children.Num() > 0)
		{
			TArray<TSharedPtr<FJsonValue>> Kids;
			for (const FSchemaDescriptor& C : Desc.Children)
			{
				Kids.Add(MakeShared<FJsonValueObject>(DescriptorToJson(C)));
			}
			O->SetArrayField(TEXT("children"), Kids);
		}
		return O;
	}

	static TArray<TSharedPtr<FJsonValue>> StringArrayToJson(const TArray<FString>& Values)
	{
		TArray<TSharedPtr<FJsonValue>> JsonValues;
		JsonValues.Reserve(Values.Num());
		for (const FString& Value : Values)
		{
			JsonValues.Add(MakeShared<FJsonValueString>(Value));
		}
		return JsonValues;
	}

	static bool TryGetStringFieldAny(
		const TSharedPtr<FJsonObject>& Params,
		const TCHAR* CanonicalName,
		std::initializer_list<const TCHAR*> Aliases,
		FString& OutValue)
	{
		if (!Params.IsValid())
		{
			return false;
		}
		if (Params->TryGetStringField(CanonicalName, OutValue) && !OutValue.IsEmpty())
		{
			return true;
		}
		for (const TCHAR* Alias : Aliases)
		{
			if (Params->TryGetStringField(Alias, OutValue) && !OutValue.IsEmpty())
			{
				return true;
			}
		}
		return false;
	}

	static TSharedPtr<FJsonObject> BuildDescribeNamespaceGuidance(
		const FString& RequestedNamespace,
		const FString& MatchStatus,
		const FString& Message)
	{
		FMonolithBulkFillRegistry& Registry = FMonolithBulkFillRegistry::Get();
		TArray<FString> Namespaces = Registry.GetRegisteredNamespaces();
		Namespaces.Sort();

		TArray<TSharedPtr<FJsonValue>> NamespaceEntries;
		NamespaceEntries.Reserve(Namespaces.Num());
		for (const FString& Namespace : Namespaces)
		{
			TSharedPtr<FJsonObject> Entry = MakeShared<FJsonObject>();
			Entry->SetStringField(TEXT("namespace"), Namespace);
			Entry->SetBoolField(TEXT("registered"), true);
			Entry->SetBoolField(TEXT("inventory_supported"), Registry.HasListTargetsAdapter(Namespace));
			NamespaceEntries.Add(MakeShared<FJsonValueObject>(Entry));
		}

		TArray<FString> NextActions;
		NextActions.Add(TEXT("bulk_fill.list_namespaces"));
		NextActions.Add(TEXT("describe.schema with target_namespace=<namespace>"));
		NextActions.Add(TEXT("describe.list_targets with target_namespace=<namespace>"));
		NextActions.Add(TEXT("describe.action_schema for action parameter schemas"));
		NextActions.Add(TEXT("monolith.discover namespace=describe"));

		TSharedPtr<FJsonObject> Out = MakeShared<FJsonObject>();
		Out->SetStringField(TEXT("match_status"), MatchStatus);
		Out->SetStringField(TEXT("message"), Message);
		Out->SetArrayField(TEXT("available_namespaces"), StringArrayToJson(Namespaces));
		Out->SetArrayField(TEXT("namespaces"), NamespaceEntries);
		Out->SetNumberField(TEXT("namespace_count"), Namespaces.Num());
		Out->SetArrayField(TEXT("next_actions"), StringArrayToJson(NextActions));
		if (!RequestedNamespace.IsEmpty())
		{
			Out->SetStringField(TEXT("requested_namespace"), RequestedNamespace);
			Out->SetBoolField(TEXT("registered"), Registry.HasAdapter(RequestedNamespace));
		}
		return Out;
	}

	// --- bulk_fill.apply ---
	static FMonolithActionResult HandleBulkFillApply(const TSharedPtr<FJsonObject>& Params)
	{
		if (!Params.IsValid())
		{
			return FMonolithActionResult::Error(TEXT("bulk_fill.apply requires params"));
		}

		FBulkFillSpec Spec;
		Params->TryGetStringField(TEXT("target_namespace"), Spec.TargetNamespace);
		Params->TryGetStringField(TEXT("target"), Spec.TargetAsset);

		const TSharedPtr<FJsonObject>* TreePtr = nullptr;
		if (Params->TryGetObjectField(TEXT("tree"), TreePtr) && TreePtr != nullptr)
		{
			Spec.Tree = *TreePtr;
		}

		TSharedPtr<FJsonValue> DryrunField = Params->TryGetField(TEXT("dry_run"));
		if (DryrunField.IsValid() && !DryrunField->TryGetBool(Spec.bDryRun))
		{
			return FMonolithActionResult::Error(TEXT("Parameter 'dry_run' must be a boolean"), FMonolithJsonUtils::ErrInvalidParams);
		}
		TSharedPtr<FJsonValue> StrictField = Params->TryGetField(TEXT("strict"));
		if (StrictField.IsValid() && !StrictField->TryGetBool(Spec.bStrict))
		{
			return FMonolithActionResult::Error(TEXT("Parameter 'strict' must be a boolean"), FMonolithJsonUtils::ErrInvalidParams);
		}

		if (Spec.TargetNamespace.IsEmpty() || Spec.TargetAsset.IsEmpty() || !Spec.Tree.IsValid())
		{
			return FMonolithActionResult::Error(
				TEXT("bulk_fill.apply requires target_namespace, target, and tree (JSON object)"),
				FMonolithJsonUtils::ErrInvalidParams);
		}

		if (!FMonolithBulkFillRegistry::Get().HasAdapter(Spec.TargetNamespace))
		{
			return FMonolithActionResult::Error(
				FString::Printf(TEXT("no bulk_fill adapter registered for namespace '%s' (Phase 0 ships dispatcher only; per-namespace adapters land in Phases 1-5)"),
					*Spec.TargetNamespace),
				FMonolithJsonUtils::ErrOptionalDepUnavailable);
		}

		const FDryRunReport Report = FMonolithBulkFillRegistry::Get().DispatchBulkFill(Spec);
		return FMonolithActionResult::Success(FMonolithDryRunGuard::ReportToJson(Report));
	}

	// --- bulk_fill.list_namespaces ---
	static FMonolithActionResult HandleBulkFillListNamespaces(const TSharedPtr<FJsonObject>& /*Params*/)
	{
		TSharedPtr<FJsonObject> Out = MakeShared<FJsonObject>();
		const TArray<FString> Namespaces = FMonolithBulkFillRegistry::Get().GetRegisteredNamespaces();

		TArray<TSharedPtr<FJsonValue>> Arr;
		Arr.Reserve(Namespaces.Num());
		for (const FString& Ns : Namespaces)
		{
			TSharedPtr<FJsonObject> Entry = MakeShared<FJsonObject>();
			Entry->SetStringField(TEXT("namespace"), Ns);
			Entry->SetBoolField(TEXT("available"), true);
			Arr.Add(MakeShared<FJsonValueObject>(Entry));
		}
		Out->SetArrayField(TEXT("namespaces"), Arr);
		Out->SetNumberField(TEXT("count"), Namespaces.Num());
		return FMonolithActionResult::Success(Out);
	}

	// --- describe.schema ---
	static FMonolithActionResult HandleDescribeSchema(const TSharedPtr<FJsonObject>& Params)
	{
		if (!Params.IsValid())
		{
			return FMonolithActionResult::Success(BuildDescribeNamespaceGuidance(
				TEXT(""),
				TEXT("namespace_index"),
				TEXT("describe.schema was called without params. Choose a registered target_namespace and optionally pass target for a target-specific schema.")));
		}

		FString TargetNamespace;
		FString Target;
		TryGetStringFieldAny(Params, TEXT("target_namespace"), { TEXT("namespace"), TEXT("domain") }, TargetNamespace);
		Params->TryGetStringField(TEXT("target"), Target);

		if (TargetNamespace.IsEmpty())
		{
			return FMonolithActionResult::Success(BuildDescribeNamespaceGuidance(
				TEXT(""),
				TEXT("namespace_index"),
				TEXT("describe.schema requires a target_namespace to return a writable-shape descriptor. Returning registered namespaces instead of a validation dead-end.")));
		}

		if (!FMonolithBulkFillRegistry::Get().HasAdapter(TargetNamespace))
		{
			return FMonolithActionResult::Success(BuildDescribeNamespaceGuidance(
				TargetNamespace,
				TEXT("no_adapter"),
				FString::Printf(TEXT("No describe adapter is registered for namespace '%s'. Use one of the available_namespaces values or describe.action_schema for action parameter schemas."), *TargetNamespace)));
		}

		const FSchemaDescriptor Root = FMonolithBulkFillRegistry::Get().DispatchDescribe(TargetNamespace, Target);
		return FMonolithActionResult::Success(DescriptorToJson(Root));
	}

	// --- describe.list_targets ---
	static FMonolithActionResult HandleDescribeListTargets(const TSharedPtr<FJsonObject>& Params)
	{
		if (!Params.IsValid())
		{
			return FMonolithActionResult::Success(BuildDescribeNamespaceGuidance(
				TEXT(""),
				TEXT("namespace_index"),
				TEXT("describe.list_targets was called without params. Choose a registered target_namespace to inspect optional inventory support.")));
		}

		FString TargetNamespace;
		TryGetStringFieldAny(Params, TEXT("target_namespace"), { TEXT("namespace"), TEXT("domain") }, TargetNamespace);

		if (TargetNamespace.IsEmpty())
		{
			return FMonolithActionResult::Success(BuildDescribeNamespaceGuidance(
				TEXT(""),
				TEXT("namespace_index"),
				TEXT("describe.list_targets requires a target_namespace to list adapter inventory. Returning registered namespaces instead of a validation dead-end.")));
		}

		FMonolithBulkFillRegistry& Registry = FMonolithBulkFillRegistry::Get();
		if (!Registry.HasAdapter(TargetNamespace))
		{
			return FMonolithActionResult::Success(BuildDescribeNamespaceGuidance(
				TargetNamespace,
				TEXT("no_adapter"),
				FString::Printf(TEXT("No describe adapter is registered for namespace '%s'. Use one of the available_namespaces values or bulk_fill.list_namespaces."), *TargetNamespace)));
		}

		const bool bInventorySupported = Registry.HasListTargetsAdapter(TargetNamespace);
		const TArray<FString> Targets = bInventorySupported
			? Registry.DispatchListTargets(TargetNamespace)
			: TArray<FString>();

		TSharedPtr<FJsonObject> Out = MakeShared<FJsonObject>();
		TArray<TSharedPtr<FJsonValue>> Arr;
		Arr.Reserve(Targets.Num());
		for (const FString& T : Targets) { Arr.Add(MakeShared<FJsonValueString>(T)); }
		Out->SetArrayField(TEXT("targets"), Arr);
		Out->SetStringField(TEXT("namespace"), TargetNamespace);
		Out->SetNumberField(TEXT("count"), Targets.Num());
		Out->SetBoolField(TEXT("inventory_supported"), bInventorySupported);
		Out->SetStringField(TEXT("contract"),
			bInventorySupported
				? TEXT("authoritative_adapter_inventory")
				: TEXT("optional_inventory_not_implemented"));
		if (!bInventorySupported)
		{
			Out->SetStringField(TEXT("message"),
				TEXT("This namespace is registered for describe.schema, but it does not expose an authoritative describe.list_targets inventory. An empty targets array means inventory is unsupported, not that the namespace has no describable targets."));
		}
		return FMonolithActionResult::Success(Out);
	}

	// --- describe::action_schema (gap #5) ---
	// Surfaces a registered ACTION's param schema (names / types / required / defaults /
	// aliases / descriptions) so callers stop trial-and-erroring param names. The data already
	// lives in FMonolithActionInfo.ParamSchema (the same object discover serializes as "params");
	// this just reads it back by (target_namespace, target_action). Closes the cause behind gaps #4/#12/#13.
	static TSharedPtr<FJsonObject> BuildDescribeActionSchemaSchema()
	{
		// RI ergonomics handover #6 (2026-05-29): canonical params are
		// `target_namespace` and `target_action`. Historical/agent-natural
		// names are kept as K2 aliases so callers that pass `{namespace, action}`
		// or `{domain, target_action}` are rewritten before required-param
		// validation fires.
		return FParamSchemaBuilder()
			.Required(TEXT("target_namespace"), TEXT("string"),
				TEXT("Namespace that owns the action (e.g. \"blueprint\", \"ui\"). Aliases: `namespace`, `domain`."),
				{ TEXT("namespace"), TEXT("domain") })
			.Required(TEXT("target_action"), TEXT("string"),
				TEXT("Action name whose param schema to return (e.g. \"add_nodes_bulk\"). Alias: `action`."),
				{ TEXT("action") })
			.Build();
	}

	static FMonolithActionResult HandleDescribeActionSchema(const TSharedPtr<FJsonObject>& Params)
	{
		// The registry's required-param validation already lists ALL missing
		// required params at once (MonolithToolRegistry.cpp ~line 319-349), so
		// callers see both `target_namespace` and `target_action` reported
		// together rather than one round-trip at a time. The handler itself
		// only re-reads the (now guaranteed) params from EffectiveParams.
		// `action` (the legacy name) has already been rewritten to
		// `target_action` by ApplyAliases — but we still fall back to
		// reading it directly as belt-and-braces back-compat in case any
		// future code path bypasses the alias rewrite.
		if (!Params.IsValid())
		{
			return FMonolithActionResult::Error(TEXT("describe.action_schema requires params"));
		}
		FString TargetNamespace;
		if (!Params->TryGetStringField(TEXT("target_namespace"), TargetNamespace) || TargetNamespace.IsEmpty())
		{
			if (!Params->TryGetStringField(TEXT("namespace"), TargetNamespace) || TargetNamespace.IsEmpty())
			{
				Params->TryGetStringField(TEXT("domain"), TargetNamespace);
			}
		}
		FString ActionName;
		if (!Params->TryGetStringField(TEXT("target_action"), ActionName) || ActionName.IsEmpty())
		{
			Params->TryGetStringField(TEXT("action"), ActionName);
		}
		if (TargetNamespace.IsEmpty() || ActionName.IsEmpty())
		{
			// Defensive: registry's required-param check should have caught this
			// already. Kept as a safety net in case the schema is bypassed.
			TArray<FString> Missing;
			if (TargetNamespace.IsEmpty()) Missing.Add(TEXT("target_namespace"));
			if (ActionName.IsEmpty())      Missing.Add(TEXT("target_action"));
			return FMonolithActionResult::Error(FString::Printf(
				TEXT("missing required parameter(s): [%s]"),
				*FString::Join(Missing, TEXT(", "))),
				FMonolithJsonUtils::ErrInvalidParams);
		}

		FMonolithToolRegistry& Reg = FMonolithToolRegistry::Get();
		const TArray<FMonolithActionInfo> Actions = Reg.GetActions(TargetNamespace);
		const FMonolithActionInfo* Found = Actions.FindByPredicate(
			[&ActionName](const FMonolithActionInfo& Info){ return Info.Action == ActionName; });

		if (!Found)
		{
			return FMonolithActionResult::Error(FString::Printf(
				TEXT("action '%s' not found in namespace '%s'. Use monolith_discover(\"%s\") to list available actions."),
				*ActionName, *TargetNamespace, *TargetNamespace));
		}

		TSharedRef<FJsonObject> Out = MakeShared<FJsonObject>();
		Out->SetStringField(TEXT("namespace"), Found->Namespace);
		Out->SetStringField(TEXT("action"), Found->Action);
		Out->SetStringField(TEXT("description"), Found->Description);
		if (!Found->Category.IsEmpty())
		{
			Out->SetStringField(TEXT("category"), Found->Category);
		}
		// Same JSON-Schema "properties" object discover serializes as "params".
		Out->SetObjectField(TEXT("params"),
			Found->ParamSchema.IsValid() ? Found->ParamSchema : MakeShared<FJsonObject>());
		return FMonolithActionResult::Success(Out);
	}
} // namespace MonolithBulkFillActionsInternal

void FMonolithBulkFillActions::RegisterAll()
{
	using namespace MonolithBulkFillActionsInternal;

	FMonolithToolRegistry& Reg = FMonolithToolRegistry::Get();

	// Reminder per ref_monolith_action_registry_api.md:
	//   RegisterAction(namespace, action, DESCRIPTION, handler, ParamSchema, Category)
	// Description is the THIRD param (NOT a keyword arg; positional).

	Reg.RegisterAction(
		TEXT("bulk_fill"),
		TEXT("apply"),
		TEXT("Apply a JSON-tree fill to an asset via the target namespace's adapter. Supports dry_run + strict."),
		FMonolithActionHandler::CreateStatic(&HandleBulkFillApply),
		BuildBulkFillSchema());

	Reg.RegisterAction(
		TEXT("bulk_fill"),
		TEXT("list_namespaces"),
		TEXT("List target_namespace values the bulk_fill registry currently knows about (one row per registered adapter)."),
		FMonolithActionHandler::CreateStatic(&HandleBulkFillListNamespaces),
		BuildBulkFillListNamespacesSchema());

	Reg.RegisterAction(
		TEXT("describe"),
		TEXT("schema"),
		TEXT("Return a rich FSchemaDescriptor tree (type names, ImportText forms, enum-value lists, clamp ranges, nested children) for a namespace-level shape or target-specific asset/action via its namespace adapter."),
		FMonolithActionHandler::CreateStatic(&HandleDescribeSchema),
		BuildDescribeSchema());

	Reg.RegisterAction(
		TEXT("describe"),
		TEXT("list_targets"),
		TEXT("List the asset paths / action names the describe adapter can introspect for a given target_namespace when the adapter implements an authoritative inventory. Registered namespaces without an inventory return inventory_supported=false and an empty targets array."),
		FMonolithActionHandler::CreateStatic(&HandleDescribeListTargets),
		BuildDescribeListTargetsSchema());

	Reg.RegisterAction(
		TEXT("describe"),
		TEXT("action_schema"),
		TEXT("Return a registered ACTION's param schema (names, types, required, defaults, aliases, descriptions) by (target_namespace, target_action). Closes param-name discoverability so callers stop trial-and-erroring param names."),
		FMonolithActionHandler::CreateStatic(&HandleDescribeActionSchema),
		BuildDescribeActionSchemaSchema());

	UE_LOG(LogMonolith, Log, TEXT("MonolithBulkFillActions: registered 2 namespaces (bulk_fill + describe) with 5 actions total"));
}

void FMonolithBulkFillActions::UnregisterAll()
{
	FMonolithToolRegistry::Get().UnregisterNamespace(TEXT("bulk_fill"));
	FMonolithToolRegistry::Get().UnregisterNamespace(TEXT("describe"));
}
