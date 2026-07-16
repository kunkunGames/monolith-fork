#include "MonolithBlueprintGraphExportActions.h"

#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "EdGraph/EdGraph.h"
#include "EdGraph/EdGraphNode.h"
#include "EdGraph/EdGraphPin.h"
#include "EdGraphUtilities.h"
#include "Kismet2/BlueprintEditorUtils.h"
#include "Kismet2/CompilerResultsLog.h"
#include "Kismet2/KismetEditorUtilities.h"
#include "Misc/PackageName.h"
#include "MonolithAssetUtils.h"
#include "MonolithBlueprintInternal.h"
#include "MonolithJsonUtils.h"
#include "MonolithParamUtils.h"
#include "ScopedTransaction.h"
#include "Serialization/ArchiveReplaceObjectRef.h"
#include "UObject/Package.h"
#include "UObject/SavePackage.h"
#include "UObject/SoftObjectPath.h"
#include "UObject/UObjectIterator.h"
#include "UObject/UObjectGlobals.h"
#include "UObject/UnrealType.h"

namespace
{
	struct FCloneRemapRule
	{
		FString From;
		FString To;
	};

	struct FCloneClassRemap
	{
		FString FromSpecifier;
		FString ToSpecifier;
		UClass* FromClass = nullptr;
		UClass* ToClass = nullptr;
	};

	struct FCloneGraphSpec
	{
		FString SourceGraphName;
		FString DestinationGraphName;
		UEdGraph* SourceGraph = nullptr;
		UEdGraph* ExistingDestinationGraph = nullptr;
		FString GraphKind;
		bool bSkip = false;
		FString SkipReason;
	};

	struct FCloneOptions
	{
		FString SourceAssetPath;
		FString DestinationAssetPath;
		FString ExistingPolicy = TEXT("fail");
		bool bCompile = true;
		bool bSave = false;
		bool bDryRun = true;
		bool bConfirm = false;
		bool bAllowEmptyRemap = false;
		TArray<FCloneGraphSpec> Graphs;
		TArray<FCloneClassRemap> ClassRemaps;
		TArray<FCloneRemapRule> ReferenceRemaps;
	};

	static FString NormalizeReferencePath(FString Path)
	{
		Path.TrimStartAndEndInline();
		Path.ReplaceInline(TEXT("\\"), TEXT("/"));
		while (Path.Len() > 1 && Path.EndsWith(TEXT("/")))
		{
			Path.LeftChopInline(1);
		}
		return Path;
	}

	static FString PackagePart(const FString& ObjectOrPackagePath)
	{
		int32 DotIndex = INDEX_NONE;
		if (ObjectOrPackagePath.FindChar(TEXT('.'), DotIndex) && DotIndex > 0)
		{
			return ObjectOrPackagePath.Left(DotIndex);
		}
		return ObjectOrPackagePath;
	}

	static bool IsValidPackageRoot(const FString& Path)
	{
		return Path.StartsWith(TEXT("/")) && Path.Len() > 1 && !Path.Contains(TEXT("//"));
	}

	static bool MatchesPrefixBoundary(const FString& Path, const FString& Prefix)
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

	static bool TryRemapReferencePath(const FString& InPath, const TArray<FCloneRemapRule>& Rules, FString& OutPath)
	{
		OutPath.Reset();
		const FString NormalizedPath = NormalizeReferencePath(InPath);
		if (NormalizedPath.IsEmpty())
		{
			return false;
		}

		const FString PkgPart = PackagePart(NormalizedPath);
		for (const FCloneRemapRule& Rule : Rules)
		{
			if (Rule.From.IsEmpty() || Rule.To.IsEmpty())
			{
				continue;
			}
			if (NormalizedPath.Equals(Rule.From, ESearchCase::CaseSensitive))
			{
				OutPath = Rule.To;
				return true;
			}
			if (PkgPart.Equals(Rule.From, ESearchCase::CaseSensitive))
			{
				const FString ObjectSuffix = NormalizedPath.Mid(PkgPart.Len());
				OutPath = Rule.To.Contains(TEXT(".")) ? Rule.To : Rule.To + ObjectSuffix;
				return true;
			}
			if (MatchesPrefixBoundary(NormalizedPath, Rule.From))
			{
				OutPath = Rule.To + NormalizedPath.Mid(Rule.From.Len());
				return true;
			}
		}
		return false;
	}

	static TArray<TSharedPtr<FJsonValue>> StringArray(const TArray<FString>& Values)
	{
		TArray<TSharedPtr<FJsonValue>> Out;
		Out.Reserve(Values.Num());
		for (const FString& Value : Values)
		{
			Out.Add(MakeShared<FJsonValueString>(Value));
		}
		return Out;
	}

	static TSharedPtr<FJsonObject> RemapRuleJson(const FCloneRemapRule& Rule)
	{
		TSharedPtr<FJsonObject> Obj = MakeShared<FJsonObject>();
		Obj->SetStringField(TEXT("from"), Rule.From);
		Obj->SetStringField(TEXT("to"), Rule.To);
		return Obj;
	}

	static TArray<TSharedPtr<FJsonValue>> RemapRulesJson(const TArray<FCloneRemapRule>& Rules)
	{
		TArray<TSharedPtr<FJsonValue>> Out;
		Out.Reserve(Rules.Num());
		for (const FCloneRemapRule& Rule : Rules)
		{
			Out.Add(MakeShared<FJsonValueObject>(RemapRuleJson(Rule)));
		}
		return Out;
	}

	static bool ParseStringMap(const TSharedPtr<FJsonObject>& Params, const FString& FieldName, TArray<FCloneRemapRule>& OutRules, FString& OutError)
	{
		const TSharedPtr<FJsonObject>* MapObject = nullptr;
		if (!Params.IsValid() || !Params->TryGetObjectField(FieldName, MapObject) || !MapObject)
		{
			return true;
		}

		for (const TPair<FString, TSharedPtr<FJsonValue>>& Pair : FMonolithJsonUtils::GetFields(*MapObject))
		{
			FString Target;
			if (!Pair.Value.IsValid() || !Pair.Value->TryGetString(Target))
			{
				OutError = FString::Printf(TEXT("%s['%s'] must be a string target path."), *FieldName, *Pair.Key);
				return false;
			}

			FCloneRemapRule Rule;
			Rule.From = NormalizeReferencePath(Pair.Key);
			Rule.To = NormalizeReferencePath(Target);
			if (Rule.From.IsEmpty() || Rule.To.IsEmpty())
			{
				OutError = FString::Printf(TEXT("%s cannot contain empty source or target paths."), *FieldName);
				return false;
			}
			OutRules.Add(MoveTemp(Rule));
		}
		return true;
	}

	static void AddReferenceRule(TArray<FCloneRemapRule>& Rules, const FString& From, const FString& To)
	{
		FCloneRemapRule Rule;
		Rule.From = NormalizeReferencePath(From);
		Rule.To = NormalizeReferencePath(To);
		if (!Rule.From.IsEmpty() && !Rule.To.IsEmpty() && Rule.From != Rule.To)
		{
			Rules.Add(MoveTemp(Rule));
		}
	}

	static UClass* ResolveClassSpecifier(const FString& InSpecifier)
	{
		FString Specifier = InSpecifier.TrimStartAndEnd();
		if (Specifier.IsEmpty())
		{
			return nullptr;
		}

		TArray<FString> Candidates;
		Candidates.Add(Specifier);
		if (Specifier.StartsWith(TEXT("/Game/")) && !Specifier.EndsWith(TEXT("_C")))
		{
			Candidates.Add(MonolithParamUtils::NormalizeBlueprintClassPath(Specifier));
		}

		for (const FString& Candidate : Candidates)
		{
			if (UClass* LoadedClass = LoadObject<UClass>(nullptr, *Candidate))
			{
				return LoadedClass;
			}
			if (UClass* LoadedClass = StaticLoadClass(UObject::StaticClass(), nullptr, *Candidate))
			{
				return LoadedClass;
			}
		}

		if (UClass* Found = FindFirstObject<UClass>(*Specifier, EFindFirstObjectOptions::NativeFirst))
		{
			return Found;
		}
		if (!Specifier.StartsWith(TEXT("U")))
		{
			if (UClass* Found = FindFirstObject<UClass>(*(TEXT("U") + Specifier), EFindFirstObjectOptions::NativeFirst))
			{
				return Found;
			}
		}
		if (!Specifier.StartsWith(TEXT("A")))
		{
			if (UClass* Found = FindFirstObject<UClass>(*(TEXT("A") + Specifier), EFindFirstObjectOptions::NativeFirst))
			{
				return Found;
			}
		}
		if ((Specifier.StartsWith(TEXT("U")) || Specifier.StartsWith(TEXT("A"))) && Specifier.Len() > 1)
		{
			if (UClass* Found = FindFirstObject<UClass>(*Specifier.Mid(1), EFindFirstObjectOptions::NativeFirst))
			{
				return Found;
			}
		}
		return nullptr;
	}

	static bool ParseClassRemaps(const TSharedPtr<FJsonObject>& Params, TArray<FCloneClassRemap>& OutClassRemaps, TArray<FCloneRemapRule>& OutReferenceRules, FString& OutError)
	{
		const TSharedPtr<FJsonObject>* ClassRemapObject = nullptr;
		if (!Params.IsValid() || !Params->TryGetObjectField(TEXT("class_remaps"), ClassRemapObject) || !ClassRemapObject)
		{
			return true;
		}

		for (const TPair<FString, TSharedPtr<FJsonValue>>& Pair : FMonolithJsonUtils::GetFields(*ClassRemapObject))
		{
			FString TargetClassSpec;
			if (!Pair.Value.IsValid() || !Pair.Value->TryGetString(TargetClassSpec))
			{
				OutError = FString::Printf(TEXT("class_remaps['%s'] must be a string class path or name."), *Pair.Key);
				return false;
			}

			FCloneClassRemap Remap;
			Remap.FromSpecifier = Pair.Key.TrimStartAndEnd();
			Remap.ToSpecifier = TargetClassSpec.TrimStartAndEnd();
			if (Remap.FromSpecifier.IsEmpty() || Remap.ToSpecifier.IsEmpty())
			{
				OutError = TEXT("class_remaps cannot contain empty source or target class specifiers.");
				return false;
			}

			Remap.FromClass = ResolveClassSpecifier(Remap.FromSpecifier);
			if (!Remap.FromClass)
			{
				OutError = FString::Printf(TEXT("Could not resolve class_remaps source '%s' as a UClass."), *Remap.FromSpecifier);
				return false;
			}

			Remap.ToClass = ResolveClassSpecifier(Remap.ToSpecifier);
			if (!Remap.ToClass)
			{
				OutError = FString::Printf(TEXT("Could not resolve class_remaps target '%s' as a UClass."), *Remap.ToSpecifier);
				return false;
			}

			AddReferenceRule(OutReferenceRules, Remap.FromClass->GetPathName(), Remap.ToClass->GetPathName());
			AddReferenceRule(
				OutReferenceRules,
				Remap.FromClass->ClassGeneratedBy ? Remap.FromClass->ClassGeneratedBy->GetPathName() : FString(),
				Remap.ToClass->ClassGeneratedBy ? Remap.ToClass->ClassGeneratedBy->GetPathName() : FString());
			OutClassRemaps.Add(MoveTemp(Remap));
		}
		return true;
	}

	static bool ParseReferenceRemaps(const TSharedPtr<FJsonObject>& Params, TArray<FCloneRemapRule>& OutRules, FString& OutError)
	{
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

		SourceRoot = NormalizeReferencePath(SourceRoot);
		DestRoot = NormalizeReferencePath(DestRoot);
		if (SourceRoot.IsEmpty() != DestRoot.IsEmpty())
		{
			OutError = TEXT("source_root and dest_root must be supplied together.");
			return false;
		}
		if (!SourceRoot.IsEmpty())
		{
			if (!IsValidPackageRoot(SourceRoot) || !IsValidPackageRoot(DestRoot))
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

				FCloneRemapRule Rule;
				Rule.From = NormalizeReferencePath(Pair.Key);
				Rule.To = NormalizeReferencePath(TargetRoot);
				if (!IsValidPackageRoot(Rule.From) || !IsValidPackageRoot(Rule.To))
				{
					OutError = FString::Printf(TEXT("Invalid root_remaps entry '%s' -> '%s'."), *Rule.From, *Rule.To);
					return false;
				}
				OutRules.Add(MoveTemp(Rule));
			}
		}

		if (!ParseStringMap(Params, TEXT("object_remaps"), OutRules, OutError))
		{
			return false;
		}

		OutRules.Sort([](const FCloneRemapRule& A, const FCloneRemapRule& B)
		{
			return A.From.Len() > B.From.Len();
		});
		return true;
	}

	static bool AddGraphSpec(TArray<FCloneGraphSpec>& Specs, FString SourceGraphName, FString DestinationGraphName, FString& OutError)
	{
		SourceGraphName.TrimStartAndEndInline();
		DestinationGraphName.TrimStartAndEndInline();
		if (SourceGraphName.IsEmpty())
		{
			OutError = TEXT("Graph specs require a non-empty source graph name.");
			return false;
		}
		if (DestinationGraphName.IsEmpty())
		{
			DestinationGraphName = SourceGraphName;
		}

		FCloneGraphSpec Spec;
		Spec.SourceGraphName = MoveTemp(SourceGraphName);
		Spec.DestinationGraphName = MoveTemp(DestinationGraphName);
		Specs.Add(MoveTemp(Spec));
		return true;
	}

	static bool ParseGraphSpecs(const TSharedPtr<FJsonObject>& Params, TArray<FCloneGraphSpec>& OutSpecs, FString& OutError)
	{
		const TSharedPtr<FJsonValue> GraphsField = Params.IsValid() ? Params->TryGetField(TEXT("graphs")) : nullptr;
		if (GraphsField.IsValid())
		{
			const TArray<TSharedPtr<FJsonValue>>* GraphValues = nullptr;
			if (!GraphsField->TryGetArray(GraphValues) || !GraphValues)
			{
				OutError = TEXT("graphs must be an array of strings or graph spec objects.");
				return false;
			}

			for (int32 Index = 0; Index < GraphValues->Num(); ++Index)
			{
				const TSharedPtr<FJsonValue>& Value = (*GraphValues)[Index];
				if (!Value.IsValid())
				{
					OutError = FString::Printf(TEXT("graphs[%d] must not be null."), Index);
					return false;
				}

				if (Value->Type == EJson::String)
				{
					if (!AddGraphSpec(OutSpecs, Value->AsString(), FString(), OutError))
					{
						return false;
					}
					continue;
				}
				if (Value->Type != EJson::Object)
				{
					OutError = FString::Printf(TEXT("graphs[%d] must be a string or object."), Index);
					return false;
				}

				const TSharedPtr<FJsonObject> GraphObject = Value->AsObject();
				FString SourceGraph;
				if (!GraphObject.IsValid() || (!GraphObject->TryGetStringField(TEXT("source_graph"), SourceGraph) && !GraphObject->TryGetStringField(TEXT("graph_name"), SourceGraph)))
				{
					OutError = FString::Printf(TEXT("graphs[%d] requires source_graph or graph_name."), Index);
					return false;
				}

				FString DestinationGraph;
				if (!GraphObject->TryGetStringField(TEXT("destination_graph"), DestinationGraph))
				{
					GraphObject->TryGetStringField(TEXT("new_name"), DestinationGraph);
				}
				if (!AddGraphSpec(OutSpecs, SourceGraph, DestinationGraph, OutError))
				{
					return false;
				}
			}
		}

		FString SingleGraphName;
		if (!MonolithParamUtils::GetOptionalStringParam(Params, TEXT("graph_name"), SingleGraphName, OutError))
		{
			return false;
		}
		if (!SingleGraphName.IsEmpty())
		{
			FString SingleNewName;
			if (!MonolithParamUtils::GetOptionalStringParam(Params, TEXT("new_name"), SingleNewName, OutError))
			{
				return false;
			}
			if (!AddGraphSpec(OutSpecs, SingleGraphName, SingleNewName, OutError))
			{
				return false;
			}
		}

		if (OutSpecs.IsEmpty())
		{
			OutError = TEXT("Provide graph_name or graphs.");
			return false;
		}

		TSet<FString> DestinationNames;
		for (const FCloneGraphSpec& Spec : OutSpecs)
		{
			if (DestinationNames.Contains(Spec.DestinationGraphName))
			{
				OutError = FString::Printf(TEXT("Duplicate destination graph in request: %s"), *Spec.DestinationGraphName);
				return false;
			}
			DestinationNames.Add(Spec.DestinationGraphName);
		}
		return true;
	}

	static bool ParseOptions(const TSharedPtr<FJsonObject>& Params, FCloneOptions& OutOptions, FString& OutError)
	{
		if (!MonolithParamUtils::GetRequiredStringParam(Params, TEXT("source_asset_path"), OutOptions.SourceAssetPath, OutError))
		{
			return false;
		}
		if (!MonolithParamUtils::GetRequiredStringParam(Params, TEXT("destination_asset_path"), OutOptions.DestinationAssetPath, OutError))
		{
			return false;
		}
		if (!MonolithParamUtils::GetOptionalStringParam(Params, TEXT("existing_policy"), OutOptions.ExistingPolicy, OutError, TEXT("fail")))
		{
			return false;
		}
		OutOptions.ExistingPolicy = OutOptions.ExistingPolicy.ToLower();
		if (OutOptions.ExistingPolicy != TEXT("fail") && OutOptions.ExistingPolicy != TEXT("replace") && OutOptions.ExistingPolicy != TEXT("skip"))
		{
			OutError = TEXT("existing_policy must be one of: fail, replace, skip.");
			return false;
		}
		if (!MonolithParamUtils::GetOptionalBoolParam(Params, TEXT("compile"), OutOptions.bCompile, OutError, true) ||
			!MonolithParamUtils::GetOptionalBoolParam(Params, TEXT("save"), OutOptions.bSave, OutError, false) ||
			!MonolithParamUtils::GetOptionalBoolParam(Params, TEXT("dry_run"), OutOptions.bDryRun, OutError, true) ||
			!MonolithParamUtils::GetOptionalBoolParam(Params, TEXT("confirm"), OutOptions.bConfirm, OutError, false) ||
			!MonolithParamUtils::GetOptionalBoolParam(Params, TEXT("allow_empty_remap"), OutOptions.bAllowEmptyRemap, OutError, false))
		{
			return false;
		}
		if (!OutOptions.bDryRun && !OutOptions.bConfirm)
		{
			OutError = TEXT("clone_graphs_with_reference_remap is mutating; pass dry_run=true to inspect the plan or confirm=true with dry_run=false to apply.");
			return false;
		}

		if (!ParseClassRemaps(Params, OutOptions.ClassRemaps, OutOptions.ReferenceRemaps, OutError) ||
			!ParseReferenceRemaps(Params, OutOptions.ReferenceRemaps, OutError) ||
			!ParseGraphSpecs(Params, OutOptions.Graphs, OutError))
		{
			return false;
		}
		if (OutOptions.ClassRemaps.IsEmpty() && OutOptions.ReferenceRemaps.IsEmpty() && !OutOptions.bAllowEmptyRemap)
		{
			OutError = TEXT("Provide class_remaps, object_remaps, root_remaps, source_root+dest_root, or pass allow_empty_remap=true explicitly.");
			return false;
		}
		return true;
	}

	static FString GraphKind(const UBlueprint* BP, const UEdGraph* Graph)
	{
		if (!BP || !Graph)
		{
			return TEXT("unknown");
		}
		UEdGraph* MutableGraph = const_cast<UEdGraph*>(Graph);
		if (BP->UbergraphPages.Contains(MutableGraph)) return TEXT("event_graph");
		if (BP->FunctionGraphs.Contains(MutableGraph)) return TEXT("function");
		if (BP->MacroGraphs.Contains(MutableGraph)) return TEXT("macro");
		if (BP->DelegateSignatureGraphs.Contains(MutableGraph)) return TEXT("delegate_signature");
		for (const FBPInterfaceDescription& Iface : BP->ImplementedInterfaces)
		{
			if (Iface.Graphs.Contains(MutableGraph))
			{
				return TEXT("interface");
			}
		}
		return TEXT("unknown");
	}

	static TSharedPtr<FJsonObject> PlanRow(const FCloneGraphSpec& Spec)
	{
		TSharedPtr<FJsonObject> Row = MakeShared<FJsonObject>();
		Row->SetStringField(TEXT("source_graph"), Spec.SourceGraphName);
		Row->SetStringField(TEXT("destination_graph"), Spec.DestinationGraphName);
		Row->SetStringField(TEXT("graph_type"), Spec.GraphKind);
		Row->SetNumberField(TEXT("source_node_count"), Spec.SourceGraph ? Spec.SourceGraph->Nodes.Num() : 0);
		Row->SetBoolField(TEXT("has_destination_collision"), Spec.ExistingDestinationGraph != nullptr);
		Row->SetStringField(TEXT("status"), Spec.bSkip ? TEXT("skipped") : TEXT("planned"));
		if (Spec.ExistingDestinationGraph)
		{
			Row->SetStringField(TEXT("existing_destination_graph"), Spec.ExistingDestinationGraph->GetName());
		}
		if (!Spec.SkipReason.IsEmpty())
		{
			Row->SetStringField(TEXT("skip_reason"), Spec.SkipReason);
		}
		return Row;
	}

	static bool BuildPlan(UBlueprint* SourceBlueprint, UBlueprint* DestinationBlueprint, FCloneOptions& Options, FString& OutError)
	{
		for (FCloneGraphSpec& Spec : Options.Graphs)
		{
			Spec.SourceGraph = MonolithBlueprintInternal::FindGraphByName(SourceBlueprint, Spec.SourceGraphName);
			if (!Spec.SourceGraph)
			{
				OutError = FString::Printf(TEXT("Source graph not found: %s"), *Spec.SourceGraphName);
				return false;
			}

			Spec.GraphKind = GraphKind(SourceBlueprint, Spec.SourceGraph);
			if (Spec.GraphKind != TEXT("function") && Spec.GraphKind != TEXT("macro"))
			{
				OutError = FString::Printf(
					TEXT("Graph '%s' is '%s'; clone_graphs_with_reference_remap supports only function and macro graphs."),
					*Spec.SourceGraphName,
					*Spec.GraphKind);
				return false;
			}

			Spec.ExistingDestinationGraph = MonolithBlueprintInternal::FindGraphByName(DestinationBlueprint, Spec.DestinationGraphName);
			if (!Spec.ExistingDestinationGraph)
			{
				continue;
			}
			if (Options.ExistingPolicy == TEXT("skip"))
			{
				Spec.bSkip = true;
				Spec.SkipReason = TEXT("destination_graph_exists");
				continue;
			}
			if (Options.ExistingPolicy != TEXT("replace"))
			{
				OutError = FString::Printf(TEXT("Destination graph already exists: %s. Use existing_policy=replace or skip."), *Spec.DestinationGraphName);
				return false;
			}

			const FString ExistingKind = GraphKind(DestinationBlueprint, Spec.ExistingDestinationGraph);
			if (ExistingKind != TEXT("function") && ExistingKind != TEXT("macro"))
			{
				OutError = FString::Printf(TEXT("Cannot replace destination graph '%s' because it is '%s'."), *Spec.DestinationGraphName, *ExistingKind);
				return false;
			}
		}
		return true;
	}

	static bool ShouldMapContainedObject(UObject* Object)
	{
		return Object && !Object->IsA<UPackage>() && !Object->HasAnyFlags(RF_Transient) && Object->GetClass()->GetName() != TEXT("MetaData");
	}

	static FString RemapRelativePathAssetName(const FString& RelativePath, const FString& SourceAssetName, const FString& DestAssetName)
	{
		if (SourceAssetName == DestAssetName)
		{
			return RelativePath;
		}
		if (RelativePath == SourceAssetName)
		{
			return DestAssetName;
		}
		if (RelativePath.StartsWith(SourceAssetName + TEXT(":")) ||
			RelativePath.StartsWith(SourceAssetName + TEXT(".")) ||
			RelativePath.StartsWith(SourceAssetName + TEXT("_")))
		{
			return DestAssetName + RelativePath.RightChop(SourceAssetName.Len());
		}
		return RelativePath;
	}

	static void AddReplacement(UObject* OldObject, UObject* NewObject, TMap<UObject*, UObject*>& Replacements)
	{
		if (OldObject && NewObject && OldObject != NewObject)
		{
			Replacements.FindOrAdd(OldObject) = NewObject;
		}
	}

	static void AddClassReplacement(UClass* OldClass, UClass* NewClass, TMap<UObject*, UObject*>& Replacements)
	{
		AddReplacement(OldClass, NewClass, Replacements);
		AddReplacement(OldClass ? OldClass->GetDefaultObject(false) : nullptr, NewClass ? NewClass->GetDefaultObject(false) : nullptr, Replacements);
	}

	static void AddPackageReplacements(const FString& SourcePackageName, const FString& DestPackageName, UPackage* SourcePackage, UPackage* DestPackage, TMap<UObject*, UObject*>& Replacements)
	{
		if (!SourcePackage || !DestPackage)
		{
			return;
		}

		// Do not FullyLoad packages here as DestPackage might be newly created in-memory,
		// which would cause data loss by reverting transient state from a leftover disk asset.
		// The graph clone operation already relies on in-memory objects properly mapped.

		const FString SourceAssetName = FPackageName::GetLongPackageAssetName(SourcePackageName);
		const FString DestAssetName = FPackageName::GetLongPackageAssetName(DestPackageName);

		TMap<FString, UObject*> DestObjects;
		for (TObjectIterator<UObject> ObjectIt; ObjectIt; ++ObjectIt)
		{
			UObject* Object = *ObjectIt;
			if (ShouldMapContainedObject(Object) && Object->GetOutermost() == DestPackage)
			{
				DestObjects.Add(Object->GetPathName(DestPackage), Object);
			}
		}

		for (TObjectIterator<UObject> ObjectIt; ObjectIt; ++ObjectIt)
		{
			UObject* SourceObject = *ObjectIt;
			if (!ShouldMapContainedObject(SourceObject) || SourceObject->GetOutermost() != SourcePackage)
			{
				continue;
			}
			const FString SourceRelative = SourceObject->GetPathName(SourcePackage);
			const FString DestRelative = RemapRelativePathAssetName(SourceRelative, SourceAssetName, DestAssetName);
			if (UObject* const* DestObject = DestObjects.Find(DestRelative))
			{
				AddReplacement(SourceObject, *DestObject, Replacements);
			}
		}
	}

	static void BuildReplacements(UBlueprint* SourceBlueprint, UBlueprint* DestinationBlueprint, const FCloneOptions& Options, TMap<UObject*, UObject*>& OutReplacements)
	{
		AddReplacement(SourceBlueprint, DestinationBlueprint, OutReplacements);
		AddClassReplacement(SourceBlueprint ? SourceBlueprint->GeneratedClass : nullptr, DestinationBlueprint ? DestinationBlueprint->GeneratedClass : nullptr, OutReplacements);
		AddClassReplacement(SourceBlueprint ? SourceBlueprint->SkeletonGeneratedClass : nullptr, DestinationBlueprint ? DestinationBlueprint->SkeletonGeneratedClass : nullptr, OutReplacements);

		if (SourceBlueprint && DestinationBlueprint)
		{
			AddPackageReplacements(
				SourceBlueprint->GetOutermost()->GetName(),
				DestinationBlueprint->GetOutermost()->GetName(),
				SourceBlueprint->GetOutermost(),
				DestinationBlueprint->GetOutermost(),
				OutReplacements);
		}

		for (const FCloneClassRemap& Remap : Options.ClassRemaps)
		{
			AddClassReplacement(Remap.FromClass, Remap.ToClass, OutReplacements);
		}
		for (const FCloneRemapRule& Rule : Options.ReferenceRemaps)
		{
			AddReplacement(FSoftObjectPath(Rule.From).TryLoad(), FSoftObjectPath(Rule.To).TryLoad(), OutReplacements);
		}
	}

	static UObject* LoadRemappedReferenceTarget(UObject* SourceObject, const TArray<FCloneRemapRule>& Remaps)
	{
		if (!SourceObject || Remaps.IsEmpty())
		{
			return nullptr;
		}

		FString TargetPath;
		if (!TryRemapReferencePath(SourceObject->GetPathName(), Remaps, TargetPath))
		{
			return nullptr;
		}

		if (UObject* TargetObject = StaticLoadObject(UObject::StaticClass(), nullptr, *TargetPath))
		{
			return TargetObject;
		}
		if (const UClass* SourceClass = Cast<UClass>(SourceObject))
		{
			UClass* RequiredBaseClass = SourceClass->GetSuperClass() ? SourceClass->GetSuperClass() : UObject::StaticClass();
			if (UClass* TargetClass = StaticLoadClass(RequiredBaseClass, nullptr, *TargetPath))
			{
				return TargetClass;
			}
		}
		return FSoftObjectPath(TargetPath).TryLoad();
	}

	static int32 AddReferencedObjectReplacements(UObject* RootObject, const TArray<FCloneRemapRule>& Remaps, TMap<UObject*, UObject*>& Replacements)
	{
		if (!RootObject || Remaps.IsEmpty())
		{
			return 0;
		}

		TArray<UObject*> ReferencedObjects;
		FReferenceFinder(ReferencedObjects, nullptr, false, true, true, true).FindReferences(RootObject);

		int32 Added = 0;
		for (UObject* ReferencedObject : ReferencedObjects)
		{
			if (!ReferencedObject || Replacements.Contains(ReferencedObject))
			{
				continue;
			}

			if (UObject* TargetObject = LoadRemappedReferenceTarget(ReferencedObject, Remaps))
			{
				AddReplacement(ReferencedObject, TargetObject, Replacements);
				++Added;
			}
		}
		return Added;
	}

	static bool FixSoftPath(FSoftObjectPath& Path, const TArray<FCloneRemapRule>& Remaps, int32& OutCount)
	{
		if (Path.IsNull())
		{
			return false;
		}
		FString NewPath;
		if (!TryRemapReferencePath(Path.ToString(), Remaps, NewPath) || NewPath == Path.ToString())
		{
			return false;
		}
		Path = FSoftObjectPath(NewPath);
		++OutCount;
		return true;
	}

	static bool FixSoftProperty(FProperty* Property, void* ValuePtr, const TArray<FCloneRemapRule>& Remaps, int32& OutCount);

	static bool FixSoftStruct(UStruct* Struct, void* StructPtr, const TArray<FCloneRemapRule>& Remaps, int32& OutCount)
	{
		bool bChanged = false;
		for (TFieldIterator<FProperty> It(Struct); It; ++It)
		{
			FProperty* Child = *It;
			if (!Child || Child->HasAnyPropertyFlags(CPF_Transient | CPF_Deprecated))
			{
				continue;
			}
			bChanged |= FixSoftProperty(Child, Child->ContainerPtrToValuePtr<void>(StructPtr), Remaps, OutCount);
		}
		return bChanged;
	}

	static bool FixSoftProperty(FProperty* Property, void* ValuePtr, const TArray<FCloneRemapRule>& Remaps, int32& OutCount)
	{
		if (!Property || !ValuePtr)
		{
			return false;
		}
		if (FSoftObjectProperty* SoftObjectProperty = CastField<FSoftObjectProperty>(Property))
		{
			FSoftObjectPtr SoftPtr = SoftObjectProperty->GetPropertyValue(ValuePtr);
			FSoftObjectPath Path = SoftPtr.ToSoftObjectPath();
			if (!FixSoftPath(Path, Remaps, OutCount))
			{
				return false;
			}
			SoftObjectProperty->SetPropertyValue(ValuePtr, FSoftObjectPtr(Path));
			return true;
		}
		if (FStructProperty* StructProperty = CastField<FStructProperty>(Property))
		{
			if (StructProperty->Struct == TBaseStructure<FSoftObjectPath>::Get())
			{
				FSoftObjectPath* Path = static_cast<FSoftObjectPath*>(ValuePtr);
				return Path ? FixSoftPath(*Path, Remaps, OutCount) : false;
			}
			return FixSoftStruct(StructProperty->Struct, ValuePtr, Remaps, OutCount);
		}
		if (FArrayProperty* ArrayProperty = CastField<FArrayProperty>(Property))
		{
			bool bChanged = false;
			FScriptArrayHelper Helper(ArrayProperty, ValuePtr);
			for (int32 Index = 0; Index < Helper.Num(); ++Index)
			{
				bChanged |= FixSoftProperty(ArrayProperty->Inner, Helper.GetRawPtr(Index), Remaps, OutCount);
			}
			return bChanged;
		}
		if (FSetProperty* SetProperty = CastField<FSetProperty>(Property))
		{
			bool bChanged = false;
			FScriptSetHelper Helper(SetProperty, ValuePtr);
			for (int32 Index = 0; Index < Helper.GetMaxIndex(); ++Index)
			{
				if (Helper.IsValidIndex(Index))
				{
					bChanged |= FixSoftProperty(SetProperty->ElementProp, Helper.GetElementPtr(Index), Remaps, OutCount);
				}
			}
			if (bChanged)
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
				bChanged |= FixSoftProperty(MapProperty->KeyProp, Helper.GetKeyPtr(Index), Remaps, OutCount);
				bChanged |= FixSoftProperty(MapProperty->ValueProp, Helper.GetValuePtr(Index), Remaps, OutCount);
			}
			if (bChanged)
			{
				Helper.Rehash();
			}
			return bChanged;
		}
		return false;
	}

	static int32 FixSoftObject(UObject* Object, const TArray<FCloneRemapRule>& Remaps)
	{
		int32 Count = 0;
		if (Object && !Remaps.IsEmpty())
		{
			FixSoftStruct(Object->GetClass(), Object, Remaps, Count);
		}
		return Count;
	}

	static int32 FixPinReferences(UEdGraphNode* Node, const TArray<FCloneRemapRule>& Remaps, const TMap<UObject*, UObject*>& Replacements)
	{
		int32 Count = 0;
		if (!Node)
		{
			return Count;
		}

		for (UEdGraphPin* Pin : Node->Pins)
		{
			if (!Pin)
			{
				continue;
			}

			if (UObject* PinSubObject = Pin->PinType.PinSubCategoryObject.Get())
			{
				if (UObject* const* Replacement = Replacements.Find(PinSubObject))
				{
					Pin->PinType.PinSubCategoryObject = *Replacement;
					++Count;
				}
			}
			if (Pin->DefaultObject)
			{
				if (UObject* const* Replacement = Replacements.Find(Pin->DefaultObject))
				{
					Pin->DefaultObject = *Replacement;
					++Count;
				}
			}

			FString NewDefaultValue;
			if (!Pin->DefaultValue.IsEmpty() && TryRemapReferencePath(Pin->DefaultValue, Remaps, NewDefaultValue))
			{
				Pin->DefaultValue = NewDefaultValue;
				++Count;
			}
		}
		return Count;
	}

	static TSharedPtr<FJsonObject> CompileBlueprintReport(UBlueprint* Blueprint)
	{
		TSharedPtr<FJsonObject> Report = MakeShared<FJsonObject>();
		FCompilerResultsLog Results;
		FKismetEditorUtilities::CompileBlueprint(Blueprint, EBlueprintCompileOptions::SkipGarbageCollection, &Results);

		TArray<TSharedPtr<FJsonValue>> Errors;
		TArray<TSharedPtr<FJsonValue>> Warnings;
		for (const TSharedRef<FTokenizedMessage>& Msg : Results.Messages)
		{
			TSharedPtr<FJsonObject> MsgObj = MakeShared<FJsonObject>();
			MsgObj->SetStringField(TEXT("message"), Msg->ToText().ToString());
			if (Msg->GetSeverity() == EMessageSeverity::Error)
			{
				Errors.Add(MakeShared<FJsonValueObject>(MsgObj));
			}
			else if (Msg->GetSeverity() == EMessageSeverity::Warning)
			{
				Warnings.Add(MakeShared<FJsonValueObject>(MsgObj));
			}
		}

		EBlueprintStatus StatusValue = BS_Unknown;
		if (Blueprint)
		{
			StatusValue = Blueprint->Status;
		}

		FString Status = TEXT("Unknown");
		switch (StatusValue)
		{
		case BS_Dirty: Status = TEXT("Dirty"); break;
		case BS_Error: Status = TEXT("Error"); break;
		case BS_UpToDate: Status = TEXT("UpToDate"); break;
		case BS_UpToDateWithWarnings: Status = TEXT("UpToDateWithWarnings"); break;
		case BS_BeingCreated: Status = TEXT("BeingCreated"); break;
		default: break;
		}

		Report->SetBoolField(TEXT("success"), Blueprint && (Blueprint->Status == BS_UpToDate || Blueprint->Status == BS_UpToDateWithWarnings));
		Report->SetStringField(TEXT("status"), Status);
		Report->SetNumberField(TEXT("error_count"), Errors.Num());
		Report->SetNumberField(TEXT("warning_count"), Warnings.Num());
		Report->SetArrayField(TEXT("errors"), Errors);
		Report->SetArrayField(TEXT("warnings"), Warnings);
		return Report;
	}

	static bool SaveDestination(UBlueprint* DestinationBlueprint, bool bSave, FString& OutFilename, FString& OutError)
	{
		OutFilename.Reset();
		OutError.Reset();
		if (!bSave)
		{
			return true;
		}
		if (!DestinationBlueprint || !DestinationBlueprint->GetOutermost())
		{
			OutError = TEXT("Destination Blueprint package is invalid.");
			return false;
		}

		UPackage* Package = DestinationBlueprint->GetOutermost();
		OutFilename = FPackageName::LongPackageNameToFilename(Package->GetName(), FPackageName::GetAssetPackageExtension());
		FSavePackageArgs SaveArgs;
		SaveArgs.TopLevelFlags = RF_Public | RF_Standalone;
		SaveArgs.SaveFlags = SAVE_NoError;
		if (!UPackage::SavePackage(Package, nullptr, *OutFilename, SaveArgs))
		{
			OutError = FString::Printf(TEXT("UPackage::SavePackage failed for '%s'."), *OutFilename);
			return false;
		}
		return true;
	}

	static bool CloneOneGraph(UBlueprint* DestinationBlueprint, const FCloneGraphSpec& Spec, const FCloneOptions& Options, TMap<UObject*, UObject*>& Replacements, TSharedPtr<FJsonObject>& OutRow, int32& OutHardFixups, int32& OutSoftFixups, int32& OutPinFixups, FString& OutError)
	{
		OutRow = MakeShared<FJsonObject>();
		OutRow->SetStringField(TEXT("source_graph"), Spec.SourceGraphName);
		OutRow->SetStringField(TEXT("destination_graph"), Spec.DestinationGraphName);
		OutRow->SetStringField(TEXT("graph_type"), Spec.GraphKind);

		if (Spec.bSkip)
		{
			OutRow->SetStringField(TEXT("status"), TEXT("skipped"));
			OutRow->SetStringField(TEXT("skip_reason"), Spec.SkipReason);
			return true;
		}

		if (Spec.ExistingDestinationGraph && Options.ExistingPolicy == TEXT("replace"))
		{
			FBlueprintEditorUtils::RemoveGraph(DestinationBlueprint, Spec.ExistingDestinationGraph, EGraphRemoveFlags::Recompile);
			OutRow->SetBoolField(TEXT("replaced_existing_graph"), true);
		}
		else
		{
			OutRow->SetBoolField(TEXT("replaced_existing_graph"), false);
		}

		UEdGraph* DuplicatedGraph = FEdGraphUtilities::CloneGraph(Spec.SourceGraph, DestinationBlueprint);
		if (!DuplicatedGraph)
		{
			OutError = FString::Printf(TEXT("Failed to duplicate source graph '%s'."), *Spec.SourceGraphName);
			return false;
		}

		DuplicatedGraph->SetFlags(RF_Transactional);
		DuplicatedGraph->Modify();
		AddReplacement(Spec.SourceGraph, DuplicatedGraph, Replacements);
		AddReferencedObjectReplacements(DuplicatedGraph, Options.ReferenceRemaps, Replacements);

		int32 LocalHardFixups = 0;
		if (!Replacements.IsEmpty())
		{
			FArchiveReplaceObjectRef<UObject> ReplaceArchive(
				DuplicatedGraph,
				Replacements,
				EArchiveReplaceObjectFlags::IgnoreOuterRef | EArchiveReplaceObjectFlags::IncludeClassGeneratedByRef);
			LocalHardFixups += static_cast<int32>(ReplaceArchive.GetCount());
		}

		int32 LocalSoftFixups = FixSoftObject(DuplicatedGraph, Options.ReferenceRemaps);
		int32 LocalPinFixups = 0;
		for (UEdGraphNode* Node : DuplicatedGraph->Nodes)
		{
			if (!Node)
			{
				continue;
			}
			Node->SetFlags(RF_Transactional);
			Node->Modify();
			Node->CreateNewGuid();
			LocalSoftFixups += FixSoftObject(Node, Options.ReferenceRemaps);
			LocalPinFixups += FixPinReferences(Node, Options.ReferenceRemaps, Replacements);
		}

		if (Spec.GraphKind == TEXT("function"))
		{
			DestinationBlueprint->FunctionGraphs.Add(DuplicatedGraph);
		}
		else
		{
			DestinationBlueprint->MacroGraphs.Add(DuplicatedGraph);
		}
		FBlueprintEditorUtils::RenameGraph(DuplicatedGraph, Spec.DestinationGraphName);

		OutHardFixups += LocalHardFixups;
		OutSoftFixups += LocalSoftFixups;
		OutPinFixups += LocalPinFixups;
		OutRow->SetStringField(TEXT("status"), TEXT("cloned"));
		OutRow->SetStringField(TEXT("new_graph"), DuplicatedGraph->GetName());
		OutRow->SetNumberField(TEXT("node_count"), DuplicatedGraph->Nodes.Num());
		OutRow->SetNumberField(TEXT("hard_reference_fixup_count"), LocalHardFixups);
		OutRow->SetNumberField(TEXT("soft_reference_fixup_count"), LocalSoftFixups);
		OutRow->SetNumberField(TEXT("pin_reference_fixup_count"), LocalPinFixups);
		return true;
	}
}

FMonolithActionResult FMonolithBlueprintGraphExportActions::HandleCloneGraphsWithReferenceRemap(const TSharedPtr<FJsonObject>& Params)
{
	FCloneOptions Options;
	FString Error;
	if (!ParseOptions(Params, Options, Error))
	{
		return FMonolithActionResult::Error(Error, FMonolithJsonUtils::ErrInvalidParams);
	}

	UBlueprint* SourceBlueprint = FMonolithAssetUtils::LoadAssetByPath<UBlueprint>(Options.SourceAssetPath);
	if (!SourceBlueprint)
	{
		SourceBlueprint = MonolithBlueprintInternal::TryLoadLevelBlueprint(Options.SourceAssetPath);
	}
	if (!SourceBlueprint)
	{
		return FMonolithActionResult::Error(FString::Printf(TEXT("Source Blueprint not found: %s"), *Options.SourceAssetPath));
	}

	UBlueprint* DestinationBlueprint = FMonolithAssetUtils::LoadAssetByPath<UBlueprint>(Options.DestinationAssetPath);
	if (!DestinationBlueprint)
	{
		DestinationBlueprint = MonolithBlueprintInternal::TryLoadLevelBlueprint(Options.DestinationAssetPath);
	}
	if (!DestinationBlueprint)
	{
		return FMonolithActionResult::Error(FString::Printf(TEXT("Destination Blueprint not found: %s"), *Options.DestinationAssetPath));
	}

	if (!BuildPlan(SourceBlueprint, DestinationBlueprint, Options, Error))
	{
		return FMonolithActionResult::Error(Error);
	}

	TArray<TSharedPtr<FJsonValue>> PlanRows;
	for (const FCloneGraphSpec& Spec : Options.Graphs)
	{
		PlanRows.Add(MakeShared<FJsonValueObject>(PlanRow(Spec)));
	}

	TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
	Result->SetStringField(TEXT("source_asset_path"), Options.SourceAssetPath);
	Result->SetStringField(TEXT("destination_asset_path"), Options.DestinationAssetPath);
	Result->SetBoolField(TEXT("dry_run"), Options.bDryRun);
	Result->SetStringField(TEXT("existing_policy"), Options.ExistingPolicy);
	Result->SetArrayField(TEXT("plan"), PlanRows);
	Result->SetArrayField(TEXT("reference_remaps"), RemapRulesJson(Options.ReferenceRemaps));
	Result->SetNumberField(TEXT("class_remap_count"), Options.ClassRemaps.Num());

	if (Options.bDryRun)
	{
		Result->SetBoolField(TEXT("changed"), false);
		Result->SetBoolField(TEXT("compiled"), false);
		Result->SetBoolField(TEXT("saved"), false);
		return FMonolithActionResult::Success(Result);
	}

	TMap<UObject*, UObject*> Replacements;
	BuildReplacements(SourceBlueprint, DestinationBlueprint, Options, Replacements);

	TArray<TSharedPtr<FJsonValue>> CloneRows;
	int32 HardFixups = 0;
	int32 SoftFixups = 0;
	int32 PinFixups = 0;
	bool bChanged = false;

	const FScopedTransaction Transaction(NSLOCTEXT("MonolithBlueprint", "CloneGraphsWithReferenceRemap", "Clone Blueprint Graphs With Reference Remap"));
	DestinationBlueprint->SetFlags(RF_Transactional);
	DestinationBlueprint->Modify();

	for (const FCloneGraphSpec& Spec : Options.Graphs)
	{
		TSharedPtr<FJsonObject> Row;
		if (!CloneOneGraph(DestinationBlueprint, Spec, Options, Replacements, Row, HardFixups, SoftFixups, PinFixups, Error))
		{
			return FMonolithActionResult::Error(Error);
		}
		if (Row.IsValid())
		{
			FString StatusString;
			if (Row->TryGetStringField(TEXT("status"), StatusString) && StatusString == TEXT("cloned"))
			{
				bChanged = true;
			}
			CloneRows.Add(MakeShared<FJsonValueObject>(Row));
		}
	}

	bool bCompiled = false;
	if (bChanged)
	{
		FBlueprintEditorUtils::MarkBlueprintAsStructurallyModified(DestinationBlueprint);
		DestinationBlueprint->GetOutermost()->MarkPackageDirty();
		if (Options.bCompile)
		{
			Result->SetObjectField(TEXT("compile_result"), CompileBlueprintReport(DestinationBlueprint));
			bCompiled = true;
		}
	}

	FString SavedFilename;
	if (!SaveDestination(DestinationBlueprint, Options.bSave && bChanged, SavedFilename, Error))
	{
		return FMonolithActionResult::Error(Error);
	}

	Result->SetBoolField(TEXT("changed"), bChanged);
	Result->SetBoolField(TEXT("compiled"), bCompiled);
	Result->SetBoolField(TEXT("saved"), !SavedFilename.IsEmpty());
	if (!SavedFilename.IsEmpty())
	{
		Result->SetStringField(TEXT("saved_filename"), SavedFilename);
	}
	Result->SetNumberField(TEXT("replacement_object_count"), Replacements.Num());
	Result->SetNumberField(TEXT("hard_reference_fixup_count"), HardFixups);
	Result->SetNumberField(TEXT("soft_reference_fixup_count"), SoftFixups);
	Result->SetNumberField(TEXT("pin_reference_fixup_count"), PinFixups);
	Result->SetArrayField(TEXT("cloned_graphs"), CloneRows);
	return FMonolithActionResult::Success(Result);
}
