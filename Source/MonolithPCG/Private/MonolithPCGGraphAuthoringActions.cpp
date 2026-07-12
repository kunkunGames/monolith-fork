#include "MonolithPCGGraphAuthoringActions.h"

#include "MonolithPCGGraphEditScope.h"
#include "MonolithPCGSettingsResolver.h"

#include "MonolithAssetUtils.h"
#include "MonolithJsonUtils.h"
#include "MonolithParamSchema.h"
#include "Reflection/MonolithDryRunGuard.h"
#include "Reflection/MonolithReflectionReader.h"
#include "Reflection/MonolithReflectionWalker.h"

#include "PCGCommon.h"
#include "PCGEdge.h"
#include "PCGGraph.h"
#include "PCGNode.h"
#include "PCGPin.h"
#include "PCGSettings.h"
#include "Data/Registry/PCGDataTypeCommon.h"

#include "AssetRegistry/AssetRegistryModule.h"
#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "Editor.h"
#include "HAL/FileManager.h"
#include "Misc/EngineVersionComparison.h"
#include "Misc/PackageName.h"
#include "Misc/Paths.h"
#include "Subsystems/EditorAssetSubsystem.h"
#include "UObject/Package.h"
#include "UObject/SavePackage.h"
#include "UObject/UnrealType.h"

namespace MonolithPCGAuthoring
{
static constexpr int32 MaxGraphNodes = 5000;
static constexpr int32 MaxGraphEdges = 20000;
static constexpr int32 MaxValidationIssues = 1000;
static constexpr int32 MaxNodeTypes = 1000;
static constexpr int32 MaxPinsPerDirection = 1024;
static constexpr int32 MaxGraphInfoResponseItems = 100000;

FMonolithActionExecutionPolicy MutationPolicy()
{
	FMonolithActionExecutionPolicy Policy;
	Policy.PolicyId = TEXT("transaction_optional");
	Policy.bDefaulted = false;
	Policy.bDirtyPackageTracking = true;
	Policy.bTransactionWrapping = true;
	// Every handler validates the mutated graph before SaveLoadedAsset. A late
	// execution-guard validator would run after persistence and cannot roll the
	// package file back if it fails, so the pre-save boundary is authoritative.
	Policy.bPostEditValidation = false;
	Policy.bEnforced = true;
	return Policy;
}

TArray<TSharedPtr<FJsonValue>> StringsToJson(const TArray<FString>& Values)
{
	TArray<TSharedPtr<FJsonValue>> Rows;
	Rows.Reserve(Values.Num());
	for (const FString& Value : Values)
	{
		Rows.Add(MakeShared<FJsonValueString>(Value));
	}
	return Rows;
}

TSharedPtr<FJsonObject> ErrorData(const FString& Field, const FString& Detail)
{
	TSharedPtr<FJsonObject> Data = MakeShared<FJsonObject>();
	Data->SetStringField(TEXT("field"), Field);
	Data->SetStringField(TEXT("detail"), Detail);
	return Data;
}

FMonolithActionResult InvalidParam(const FString& Field, const FString& Detail)
{
	return FMonolithActionResult::Error(Detail, FMonolithJsonUtils::ErrInvalidParams)
		.WithErrorData(ErrorData(Field, Detail));
}

bool ReadRequiredString(const TSharedPtr<FJsonObject>& Params, const TCHAR* Field, FString& OutValue, FString& OutError)
{
	OutValue.Reset();
	if (!Params.IsValid() || !Params->TryGetStringField(Field, OutValue))
	{
		OutError = FString::Printf(TEXT("%s must be a string"), Field);
		return false;
	}
	OutValue.TrimStartAndEndInline();
	if (OutValue.IsEmpty())
	{
		OutError = FString::Printf(TEXT("%s must not be empty"), Field);
		return false;
	}
	return true;
}

bool ReadOptionalBool(const TSharedPtr<FJsonObject>& Params, const TCHAR* Field, bool DefaultValue, bool& OutValue,
					  FString& OutError)
{
	OutValue = DefaultValue;
	if (!Params.IsValid() || !Params->HasField(Field))
	{
		return true;
	}
	if (!Params->TryGetBoolField(Field, OutValue))
	{
		OutError = FString::Printf(TEXT("%s must be a boolean"), Field);
		return false;
	}
	return true;
}

bool ReadOptionalInt(const TSharedPtr<FJsonObject>& Params, const TCHAR* Field, int32 DefaultValue, int32 MinValue,
					 int32 MaxValue, int32& OutValue, FString& OutError)
{
	double Number = static_cast<double>(DefaultValue);
	if (Params.IsValid() && Params->HasField(Field) && !Params->TryGetNumberField(Field, Number))
	{
		OutError = FString::Printf(TEXT("%s must be a number"), Field);
		return false;
	}
	const int64 IntegralValue = static_cast<int64>(Number);
	if (!FMath::IsNearlyEqual(Number, static_cast<double>(IntegralValue)) || IntegralValue < MinValue ||
		IntegralValue > MaxValue)
	{
		OutError = FString::Printf(TEXT("%s must be an integer in range %d..%d"), Field, MinValue, MaxValue);
		return false;
	}
	OutValue = static_cast<int32>(IntegralValue);
	return true;
}

bool ReadStringArray(const TSharedPtr<FJsonObject>& Params, const TCHAR* Field, TArray<FString>& OutValues,
					 FString& OutError)
{
	OutValues.Reset();
	if (!Params.IsValid() || !Params->HasField(Field))
	{
		return true;
	}

	const TArray<TSharedPtr<FJsonValue>>* Values = nullptr;
	if (!Params->TryGetArrayField(Field, Values) || !Values)
	{
		OutError = FString::Printf(TEXT("%s must be an array of strings"), Field);
		return false;
	}
	for (const TSharedPtr<FJsonValue>& Value : *Values)
	{
		FString StringValue;
		if (!Value.IsValid() || !Value->TryGetString(StringValue))
		{
			OutError = FString::Printf(TEXT("%s must be an array of strings"), Field);
			return false;
		}
		StringValue.TrimStartAndEndInline();
		if (!StringValue.IsEmpty())
		{
			OutValues.AddUnique(StringValue);
		}
	}
	return true;
}

bool IsProjectOwnedPackage(const FString& PackageName)
{
	FString Filename;
	if (!FPackageName::TryConvertLongPackageNameToFilename(PackageName, Filename,
														   FPackageName::GetAssetPackageExtension()))
	{
		return false;
	}

	Filename = FPaths::ConvertRelativePathToFull(Filename);
	FPaths::NormalizeFilename(Filename);
	FString ProjectDirectory = FPaths::ConvertRelativePathToFull(FPaths::ProjectDir());
	FPaths::NormalizeDirectoryName(ProjectDirectory);
	return Filename.StartsWith(ProjectDirectory + TEXT("/"), ESearchCase::IgnoreCase);
}

bool NormalizeGraphPath(const FString& InputPath, FString& OutPackageName, FString& OutObjectPath, FString& OutError)
{
	FString ResolvedPath = FMonolithAssetUtils::ResolveAssetPath(InputPath);
	ResolvedPath.TrimStartAndEndInline();
	if (ResolvedPath.Contains(TEXT(":")))
	{
		OutError = TEXT("asset_path must identify a top-level PCG graph asset, not "
						"a subobject");
		return false;
	}

	FString RequestedObjectName;
	int32 DotIndex = INDEX_NONE;
	if (ResolvedPath.FindLastChar(TEXT('.'), DotIndex))
	{
		OutPackageName = ResolvedPath.Left(DotIndex);
		RequestedObjectName = ResolvedPath.Mid(DotIndex + 1);
	}
	else
	{
		OutPackageName = ResolvedPath;
	}

	if (!FPackageName::IsValidLongPackageName(OutPackageName) || !IsProjectOwnedPackage(OutPackageName))
	{
		OutError = FString::Printf(TEXT("asset_path must resolve to a mounted "
										"package inside the current project: %s"),
								   *InputPath);
		return false;
	}

	const FString AssetName = FPackageName::GetLongPackageAssetName(OutPackageName);
	if (!RequestedObjectName.IsEmpty() && !RequestedObjectName.Equals(AssetName, ESearchCase::CaseSensitive))
	{
		OutError = FString::Printf(TEXT("PCG graph object name must match its package name ('%s'): %s"), *AssetName,
								   *InputPath);
		return false;
	}

	OutObjectPath = OutPackageName + TEXT(".") + AssetName;
	return true;
}

bool LoadGraph(const FString& InputPath, UPCGGraph*& OutGraph, FString& OutObjectPath, FString& OutError)
{
	OutGraph = nullptr;
	FString PackageName;
	if (!NormalizeGraphPath(InputPath, PackageName, OutObjectPath, OutError))
	{
		return false;
	}

	FString ResolvedPath;
	if (!FMonolithAssetUtils::TryLoadAssetByPath<UPCGGraph>(OutObjectPath, OutGraph, ResolvedPath, OutError))
	{
		OutError = FString::Printf(TEXT("Could not load PCG graph '%s': %s"), *OutObjectPath, *OutError);
		return false;
	}
	return true;
}

bool SaveGraph(UPCGGraph* Graph, bool bSave, bool& bOutSaved, FString& OutFilename, FString& OutError)
{
	bOutSaved = false;
	OutFilename.Reset();
	if (!Graph || !Graph->GetPackage())
	{
		OutError = TEXT("PCG graph has no package");
		return false;
	}
	if (!bSave)
	{
		return true;
	}

	UPackage* Package = Graph->GetPackage();
	const FString PackageName = Package->GetName();
	OutFilename = FPackageName::LongPackageNameToFilename(PackageName, FPackageName::GetAssetPackageExtension());
	UEditorAssetSubsystem* AssetSubsystem = GEditor ? GEditor->GetEditorSubsystem<UEditorAssetSubsystem>() : nullptr;
	if (!AssetSubsystem)
	{
		OutError = TEXT("EditorAssetSubsystem is unavailable; PCG graph was not saved");
		return false;
	}
	bOutSaved = AssetSubsystem->SaveLoadedAsset(Graph, false);
	if (!bOutSaved)
	{
		OutError = FString::Printf(TEXT("EditorAssetSubsystem::SaveLoadedAsset failed for '%s'"), *OutFilename);
		return false;
	}
	const int64 SavedFileSize = IFileManager::Get().FileSize(*OutFilename);
	if (SavedFileSize <= 0)
	{
		bOutSaved = false;
		OutError = FString::Printf(
			TEXT("PCG graph save reported success but the package file is missing or empty: '%s'"), *OutFilename);
		return false;
	}
	return true;
}

FString NodeId(const UPCGGraph* Graph, const UPCGNode* Node)
{
	if (!Graph || !Node)
	{
		return FString();
	}
	if (Node == Graph->GetInputNode())
	{
		return TEXT("__input__");
	}
	if (Node == Graph->GetOutputNode())
	{
		return TEXT("__output__");
	}
	return Node->GetName();
}

bool IsGraphNode(const UPCGGraph* Graph, const UPCGNode* Node)
{
	return Graph && Node && Node->GetGraph() == Graph && Graph->Contains(const_cast<UPCGNode*>(Node));
}

TArray<UPCGEdge*> GetGraphEdges(UPCGGraph* Graph)
{
	TSet<UPCGEdge*> UniqueEdges;
	if (!Graph)
	{
		return {};
	}

	auto AppendNodeEdges = [&UniqueEdges](UPCGNode* Node)
	{
		if (!Node)
		{
			return;
		}
		for (const UPCGPin* Pin : Node->GetInputPins())
		{
			if (Pin)
			{
				for (UPCGEdge* Edge : Pin->Edges)
				{
					if (Edge)
					{
						UniqueEdges.Add(Edge);
					}
				}
			}
		}
		for (const UPCGPin* Pin : Node->GetOutputPins())
		{
			if (Pin)
			{
				for (UPCGEdge* Edge : Pin->Edges)
				{
					if (Edge)
					{
						UniqueEdges.Add(Edge);
					}
				}
			}
		}
	};

	AppendNodeEdges(Graph->GetInputNode());
	AppendNodeEdges(Graph->GetOutputNode());
	for (UPCGNode* Node : Graph->GetNodes())
	{
		AppendNodeEdges(Node);
	}

	TArray<UPCGEdge*> Edges = UniqueEdges.Array();
	Edges.Sort(
		[Graph](const UPCGEdge& A, const UPCGEdge& B)
		{
			auto EdgeKey = [Graph](const UPCGEdge& Edge)
			{
				const UPCGPin* SourcePin = Edge.InputPin.Get();
				const UPCGPin* TargetPin = Edge.OutputPin.Get();
				return NodeId(Graph, SourcePin ? SourcePin->Node.Get() : nullptr) + TEXT("|") +
					   (SourcePin ? SourcePin->Properties.Label.ToString() : FString()) + TEXT("|") +
					   NodeId(Graph, TargetPin ? TargetPin->Node.Get() : nullptr) + TEXT("|") +
					   (TargetPin ? TargetPin->Properties.Label.ToString() : FString());
			};
			return EdgeKey(A) < EdgeKey(B);
		});
	return Edges;
}

UPCGNode* ResolveNode(UPCGGraph* Graph, const FString& Identifier, bool bAllowSpecialNodes, FString& OutError)
{
	if (!Graph)
	{
		OutError = TEXT("PCG graph is null");
		return nullptr;
	}

	FString Token = Identifier;
	Token.TrimStartAndEndInline();
	if (bAllowSpecialNodes && (Token.Equals(TEXT("__input__"), ESearchCase::IgnoreCase) ||
							   Token.Equals(TEXT("input"), ESearchCase::IgnoreCase)))
	{
		return Graph->GetInputNode();
	}
	if (bAllowSpecialNodes && (Token.Equals(TEXT("__output__"), ESearchCase::IgnoreCase) ||
							   Token.Equals(TEXT("output"), ESearchCase::IgnoreCase)))
	{
		return Graph->GetOutputNode();
	}

	TArray<UPCGNode*> ExactMatches;
	TArray<UPCGNode*> TitleMatches;
	for (UPCGNode* Node : Graph->GetNodes())
	{
		if (!Node)
		{
			continue;
		}
		if (Node->GetName().Equals(Token, ESearchCase::IgnoreCase) ||
			Node->GetPathName().Equals(Token, ESearchCase::IgnoreCase))
		{
			ExactMatches.Add(Node);
		}
#if WITH_EDITOR
		if (Node->HasAuthoredTitle() && Node->GetAuthoredTitleName().ToString().Equals(Token, ESearchCase::IgnoreCase))
		{
			TitleMatches.Add(Node);
		}
#endif
	}

	const TArray<UPCGNode*>& Matches = ExactMatches.IsEmpty() ? TitleMatches : ExactMatches;
	if (Matches.Num() == 1)
	{
		return Matches[0];
	}
	if (Matches.Num() > 1)
	{
		OutError = FString::Printf(TEXT("PCG node identifier '%s' is ambiguous; use the exact node_id"), *Identifier);
	}
	else
	{
		OutError = FString::Printf(TEXT("PCG node not found: %s"), *Identifier);
	}
	return nullptr;
}

TArray<UPCGNode*> FindNodesByAuthoredTitle(UPCGGraph* Graph, const FString& Title, const UPCGNode* ExcludedNode = nullptr)
{
	TArray<UPCGNode*> Matches;
	if (!Graph || Title.IsEmpty())
	{
		return Matches;
	}
	for (UPCGNode* Node : Graph->GetNodes())
	{
		if (Node && Node != ExcludedNode && Node->HasAuthoredTitle() &&
			Node->GetAuthoredTitleName().ToString().Equals(Title, ESearchCase::IgnoreCase))
		{
			Matches.Add(Node);
		}
	}
	return Matches;
}

TSharedPtr<FJsonValue> BoundJsonValue(const TSharedPtr<FJsonValue>& Value, int32 Depth, int32 ArrayLimit,
									  int32 ObjectLimit, int32& RemainingItems, bool& bOutTruncated)
{
	if (RemainingItems <= 0)
	{
		bOutTruncated = true;
		return MakeShared<FJsonValueNull>();
	}
	--RemainingItems;
	if (!Value.IsValid() || Depth <= 0)
	{
		bOutTruncated |= Value.IsValid();
		return MakeShared<FJsonValueNull>();
	}

	switch (Value->Type)
	{
	case EJson::String:
	{
		FString StringValue = Value->AsString();
		if (StringValue.Len() > 1024)
		{
			StringValue.LeftInline(1024);
			bOutTruncated = true;
		}
		return MakeShared<FJsonValueString>(StringValue);
	}
	case EJson::Array:
	{
		const TArray<TSharedPtr<FJsonValue>>& Source = Value->AsArray();
		TArray<TSharedPtr<FJsonValue>> Bounded;
		Bounded.Reserve(FMath::Min(Source.Num(), ArrayLimit));
		for (int32 Index = 0; Index < Source.Num() && Index < ArrayLimit && RemainingItems > 0; ++Index)
		{
			Bounded.Add(BoundJsonValue(
				Source[Index], Depth - 1, ArrayLimit, ObjectLimit, RemainingItems, bOutTruncated));
		}
		bOutTruncated |= Source.Num() > Bounded.Num();
		return MakeShared<FJsonValueArray>(Bounded);
	}
		case EJson::Object:
		{
			const TSharedPtr<FJsonObject> Source = Value->AsObject();
			TSharedPtr<FJsonObject> Bounded = MakeShared<FJsonObject>();
			TArray<TPair<FString, TSharedPtr<FJsonValue>>> Fields;
			if (Source.IsValid())
			{
				Fields.Reserve(Source->Values.Num());
				for (const auto& Pair : Source->Values)
				{
#if UE_VERSION_NEWER_THAN_OR_EQUAL(5, 8, 0)
					Fields.Emplace(FString(Pair.Key.ToView()), Pair.Value);
#else
					Fields.Emplace(Pair.Key, Pair.Value);
#endif
				}
				Fields.Sort([](const auto& A, const auto& B) { return A.Key < B.Key; });
			}
			for (int32 Index = 0; Index < Fields.Num() && Index < ObjectLimit && RemainingItems > 0; ++Index)
			{
				Bounded->SetField(Fields[Index].Key,
								  BoundJsonValue(Fields[Index].Value, Depth - 1, ArrayLimit, ObjectLimit,
									RemainingItems, bOutTruncated));
			}
			bOutTruncated |= Fields.Num() > ObjectLimit ||
				(RemainingItems <= 0 && Fields.Num() > Bounded->Values.Num());
			return MakeShared<FJsonValueObject>(Bounded);
		}
	default:
		return Value;
	}
}

bool IsEditableSettingsProperty(const FProperty* Property)
{
	return Property && Property->HasAnyPropertyFlags(CPF_Edit) &&
		   !Property->HasAnyPropertyFlags(CPF_Transient | CPF_Deprecated | CPF_EditConst | CPF_DisableEditOnInstance);
}

bool CanEditSettingsProperty(const UPCGSettings* Settings, const FProperty* Property)
{
	// UPCGSettings narrows its UObject overrides to protected. Calling through
	// the public UObject contract preserves virtual dispatch to those overrides.
	return Settings && Property && static_cast<const UObject*>(Settings)->CanEditChange(Property);
}

bool CanEditSettingsPropertyChain(const UPCGSettings* Settings, const FEditPropertyChain& PropertyChain)
{
	return Settings && static_cast<const UObject*>(Settings)->CanEditChange(PropertyChain);
}

TSharedPtr<FJsonObject> SerializeSettings(UPCGSettings* Settings, const TArray<FString>& RequestedFields,
										  int32 PropertyLimit, int32 ArrayLimit, int32& RemainingItems,
										  bool& bOutTruncated)
{
	TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
	bOutTruncated = false;
	if (!Settings)
	{
		return Result;
	}

	TArray<FProperty*> Properties;
	for (TFieldIterator<FProperty> It(Settings->GetClass()); It; ++It)
	{
		FProperty* Property = *It;
		if (!IsEditableSettingsProperty(Property) || !CanEditSettingsProperty(Settings, Property))
		{
			continue;
		}

		bool bRequested = RequestedFields.IsEmpty();
		for (const FString& RequestedField : RequestedFields)
		{
			if (Property->GetName().Equals(RequestedField, ESearchCase::IgnoreCase))
			{
				bRequested = true;
				break;
			}
		}
		if (bRequested)
		{
			Properties.Add(Property);
		}
	}
	Properties.Sort([](const FProperty& A, const FProperty& B) { return A.GetName() < B.GetName(); });

	for (int32 Index = 0; Index < Properties.Num() && Index < PropertyLimit; ++Index)
	{
		if (RemainingItems <= 0)
		{
			bOutTruncated = true;
			break;
		}
		FProperty* Property = Properties[Index];
		const void* ValuePtr = Property->ContainerPtrToValuePtr<void>(Settings);
		Result->SetField(Property->GetName(),
						 BoundJsonValue(FMonolithReflectionReader::PropertyToJsonValue(Property, ValuePtr, Settings), 8,
										ArrayLimit, PropertyLimit, RemainingItems, bOutTruncated));
	}
	bOutTruncated |= Properties.Num() > PropertyLimit;
	return Result;
}

TArray<TSharedPtr<FJsonValue>> SerializePins(const TArray<TObjectPtr<UPCGPin>>& Pins, const TCHAR* Direction,
										 int32 PinLimit, int32& RemainingItems, bool& bOutTruncated)
{
	TArray<TSharedPtr<FJsonValue>> Rows;
	Rows.Reserve(FMath::Min(Pins.Num(), PinLimit));
	int32 ValidPinCount = 0;
	for (const UPCGPin* Pin : Pins)
	{
		if (!Pin)
		{
			continue;
		}
		++ValidPinCount;
		if (Rows.Num() >= PinLimit || RemainingItems <= 0)
		{
			continue;
		}
		--RemainingItems;
		TSharedPtr<FJsonObject> Row = MakeShared<FJsonObject>();
		Row->SetStringField(TEXT("label"), Pin->Properties.Label.ToString());
		Row->SetStringField(TEXT("direction"), Direction);
		Row->SetBoolField(TEXT("connected"), Pin->IsConnected());
		Row->SetNumberField(TEXT("edge_count"), Pin->EdgeCount());
		Row->SetBoolField(TEXT("allows_multiple_connections"), Pin->AllowsMultipleConnections());
		Row->SetBoolField(TEXT("allows_multiple_data"), Pin->AllowsMultipleData());
		Row->SetBoolField(TEXT("required"), Pin->Properties.IsRequiredPin());
		Row->SetBoolField(TEXT("advanced"), Pin->Properties.IsAdvancedPin());
		Rows.Add(MakeShared<FJsonValueObject>(Row));
	}
	bOutTruncated = ValidPinCount > Rows.Num();
	return Rows;
}

TSharedPtr<FJsonObject> SerializeNode(UPCGGraph* Graph, UPCGNode* Node, bool bIncludeSettings,
									  const TArray<FString>& SettingsFields, int32 PropertyLimit, int32 ArrayLimit,
									  int32 PinLimit, int32& RemainingItems, bool& bOutTruncated)
{
	bOutTruncated = false;
	TSharedPtr<FJsonObject> Row = MakeShared<FJsonObject>();
	Row->SetStringField(TEXT("node_id"), NodeId(Graph, Node));
	Row->SetStringField(TEXT("node_path"), Node ? Node->GetPathName() : FString());
	Row->SetStringField(TEXT("kind"), Node == Graph->GetInputNode()	   ? TEXT("graph_input")
									  : Node == Graph->GetOutputNode() ? TEXT("graph_output")
																	   : TEXT("element"));

	if (!Node)
	{
		return Row;
	}

#if WITH_EDITOR
	Row->SetStringField(TEXT("authored_title"), Node->GetAuthoredTitleName().ToString());
	Row->SetStringField(TEXT("display_title"), Node->GetNodeTitle(EPCGNodeTitleType::ListView).ToString());
	int32 PositionX = 0;
	int32 PositionY = 0;
	Node->GetNodePosition(PositionX, PositionY);
	TSharedPtr<FJsonObject> Position = MakeShared<FJsonObject>();
	Position->SetNumberField(TEXT("x"), PositionX);
	Position->SetNumberField(TEXT("y"), PositionY);
	Row->SetObjectField(TEXT("position"), Position);
#endif

	UPCGSettings* Settings = Node->GetSettings();
	Row->SetBoolField(TEXT("has_settings"), Settings != nullptr);
	if (Settings)
	{
		Row->SetStringField(TEXT("settings_class"), Settings->GetClass()->GetName());
		Row->SetStringField(TEXT("settings_class_path"), Settings->GetClass()->GetClassPathName().ToString());
		Row->SetBoolField(TEXT("enabled"), Settings->bEnabled);
		if (bIncludeSettings)
		{
			bool bSettingsTruncated = false;
			Row->SetObjectField(TEXT("settings"), SerializeSettings(
				Settings, SettingsFields, PropertyLimit, ArrayLimit, RemainingItems, bSettingsTruncated));
			Row->SetBoolField(TEXT("settings_truncated"), bSettingsTruncated);
			bOutTruncated |= bSettingsTruncated;
		}
	}

	bool bInputPinsTruncated = false;
	bool bOutputPinsTruncated = false;
	Row->SetArrayField(TEXT("input_pins"),
		SerializePins(Node->GetInputPins(), TEXT("input"), PinLimit, RemainingItems, bInputPinsTruncated));
	Row->SetArrayField(TEXT("output_pins"),
		SerializePins(Node->GetOutputPins(), TEXT("output"), PinLimit, RemainingItems, bOutputPinsTruncated));
	Row->SetBoolField(TEXT("input_pins_truncated"), bInputPinsTruncated);
	Row->SetBoolField(TEXT("output_pins_truncated"), bOutputPinsTruncated);
	bOutTruncated |= bInputPinsTruncated || bOutputPinsTruncated;
	return Row;
}

TSharedPtr<FJsonObject> SerializeEdge(UPCGGraph* Graph, UPCGEdge* Edge)
{
	TSharedPtr<FJsonObject> Row = MakeShared<FJsonObject>();
	const UPCGPin* SourcePin = Edge ? Edge->InputPin.Get() : nullptr;
	const UPCGPin* TargetPin = Edge ? Edge->OutputPin.Get() : nullptr;
	const UPCGNode* SourceNode = SourcePin ? SourcePin->Node.Get() : nullptr;
	const UPCGNode* TargetNode = TargetPin ? TargetPin->Node.Get() : nullptr;
	auto ContainsEdge = [Edge](const UPCGPin* Pin)
	{
		if (!Pin || !Edge)
		{
			return false;
		}
		for (const UPCGEdge* AttachedEdge : Pin->Edges)
		{
			if (AttachedEdge == Edge)
			{
				return true;
			}
		}
		return false;
	};
	const bool bValid = Edge && Edge->IsValid() && SourcePin && TargetPin &&
		IsGraphNode(Graph, SourceNode) && IsGraphNode(Graph, TargetNode) && SourcePin->IsOutputPin() &&
		!TargetPin->IsOutputPin() && SourceNode->GetOutputPin(SourcePin->Properties.Label) == SourcePin &&
		TargetNode->GetInputPin(TargetPin->Properties.Label) == TargetPin && ContainsEdge(SourcePin) &&
		ContainsEdge(TargetPin);
	Row->SetBoolField(TEXT("valid"), bValid);
	Row->SetStringField(TEXT("source_node"), NodeId(Graph, SourcePin ? SourcePin->Node.Get() : nullptr));
	Row->SetStringField(TEXT("source_pin"), SourcePin ? SourcePin->Properties.Label.ToString() : FString());
	Row->SetStringField(TEXT("target_node"), NodeId(Graph, TargetPin ? TargetPin->Node.Get() : nullptr));
	Row->SetStringField(TEXT("target_pin"), TargetPin ? TargetPin->Properties.Label.ToString() : FString());
	return Row;
}

TSharedPtr<FJsonObject> BuildGraphInfo(UPCGGraph* Graph, const FString& ObjectPath, bool bIncludeSettings,
									   const TArray<FString>& SettingsFields, int32 PropertyLimit, int32 ArrayLimit,
									   int32 NodeLimit, int32 EdgeLimit, int32 PinLimit, int32 ResponseItemLimit)
{
	TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
	Result->SetStringField(TEXT("namespace"), TEXT("pcg"));
	Result->SetStringField(TEXT("action"), TEXT("get_pcg_graph_info"));
	Result->SetStringField(TEXT("asset_path"), ObjectPath);
	Result->SetStringField(TEXT("package_name"), Graph->GetPackage()->GetName());
	Result->SetStringField(TEXT("class"), Graph->GetClass()->GetName());
	Result->SetBoolField(TEXT("dirty"), Graph->GetPackage()->IsDirty());

	int32 RemainingItems = ResponseItemLimit;
	bool bResponseTruncated = false;
	TArray<TSharedPtr<FJsonValue>> Nodes;
	Nodes.Reserve(FMath::Min(Graph->GetNodes().Num() + 2, NodeLimit + 2));
	auto AppendNode = [&](UPCGNode* Node)
	{
		if (RemainingItems <= 0)
		{
			bResponseTruncated = true;
			return false;
		}
		--RemainingItems;
		bool bNodeTruncated = false;
		Nodes.Add(MakeShared<FJsonValueObject>(SerializeNode(Graph, Node, bIncludeSettings, SettingsFields,
			PropertyLimit, ArrayLimit, PinLimit, RemainingItems, bNodeTruncated)));
		bResponseTruncated |= bNodeTruncated;
		return true;
	};
	int32 ReturnedSpecialNodeCount = 0;
	ReturnedSpecialNodeCount += AppendNode(Graph->GetInputNode()) ? 1 : 0;
	ReturnedSpecialNodeCount += AppendNode(Graph->GetOutputNode()) ? 1 : 0;
	int32 ReturnedElementNodeCount = 0;
	for (int32 Index = 0; Index < Graph->GetNodes().Num() && Index < NodeLimit; ++Index)
	{
		if (!AppendNode(Graph->GetNodes()[Index]))
		{
			break;
		}
		++ReturnedElementNodeCount;
	}

	TArray<UPCGEdge*> GraphEdges = GetGraphEdges(Graph);
	TArray<TSharedPtr<FJsonValue>> Edges;
	Edges.Reserve(FMath::Min(GraphEdges.Num(), EdgeLimit));
	for (int32 Index = 0; Index < GraphEdges.Num() && Index < EdgeLimit && RemainingItems > 0; ++Index)
	{
		--RemainingItems;
		Edges.Add(MakeShared<FJsonValueObject>(SerializeEdge(Graph, GraphEdges[Index])));
	}

	Result->SetNumberField(TEXT("element_node_count"), Graph->GetNodes().Num());
	Result->SetNumberField(TEXT("returned_element_node_count"), ReturnedElementNodeCount);
	Result->SetNumberField(TEXT("returned_special_node_count"), ReturnedSpecialNodeCount);
	Result->SetNumberField(TEXT("edge_count"), GraphEdges.Num());
	Result->SetNumberField(TEXT("returned_edge_count"), Edges.Num());
	const bool bNodesTruncated = Graph->GetNodes().Num() > ReturnedElementNodeCount || ReturnedSpecialNodeCount < 2;
	const bool bEdgesTruncated = GraphEdges.Num() > Edges.Num();
	bResponseTruncated |= bNodesTruncated || bEdgesTruncated;
	Result->SetBoolField(TEXT("nodes_truncated"), bNodesTruncated);
	Result->SetBoolField(TEXT("edges_truncated"), bEdgesTruncated);
	Result->SetNumberField(TEXT("edge_limit"), EdgeLimit);
	Result->SetNumberField(TEXT("pin_limit"), PinLimit);
	Result->SetNumberField(TEXT("response_item_limit"), ResponseItemLimit);
	Result->SetNumberField(TEXT("returned_response_item_count"), ResponseItemLimit - RemainingItems);
	Result->SetBoolField(TEXT("response_truncated"), bResponseTruncated);
	Result->SetArrayField(TEXT("nodes"), Nodes);
	Result->SetArrayField(TEXT("edges"), Edges);
	return Result;
}

bool AreConnected(const UPCGPin* SourcePin, const UPCGPin* TargetPin)
{
	if (!SourcePin || !TargetPin)
	{
		return false;
	}
	for (const UPCGEdge* Edge : SourcePin->Edges)
	{
		if (Edge && Edge->InputPin == SourcePin && Edge->OutputPin == TargetPin)
		{
			return true;
		}
	}
	return false;
}

bool WouldCreateCycle(const UPCGNode* SourceNode, const UPCGNode* TargetNode)
{
	if (!SourceNode || !TargetNode || SourceNode == TargetNode)
	{
		return true;
	}

	TSet<const UPCGNode*> VisitedNodes;
	TArray<const UPCGNode*> PendingNodes;
	PendingNodes.Add(SourceNode);
	while (!PendingNodes.IsEmpty())
	{
		const UPCGNode* Node = PendingNodes.Pop(EAllowShrinking::No);
		if (!Node || VisitedNodes.Contains(Node))
		{
			continue;
		}
		if (Node == TargetNode)
		{
			return true;
		}

		VisitedNodes.Add(Node);
		for (const UPCGPin* InputPin : Node->GetInputPins())
		{
			if (!InputPin)
			{
				continue;
			}
			for (const UPCGEdge* Edge : InputPin->Edges)
			{
				if (Edge)
				{
					if (const UPCGPin* UpstreamPin = Edge->GetOtherPin(InputPin))
					{
						PendingNodes.Add(UpstreamPin->Node.Get());
					}
				}
			}
		}
	}
	return false;
}

struct FSettingsWriteUnit
{
	TSharedPtr<FJsonObject> Tree;
	TArray<FProperty*> PropertyChain;
	FString Path;
};

FEditPropertyChain MakeEditPropertyChain(const TArray<FProperty*>& Properties)
{
	FEditPropertyChain Chain;
	for (FProperty* Property : Properties)
	{
		Chain.AddTail(Property);
	}
	if (!Properties.IsEmpty())
	{
		Chain.SetActivePropertyNode(Properties.Last());
		Chain.SetActiveMemberPropertyNode(Properties[0]);
	}
	return Chain;
}

TSharedPtr<FJsonObject> BuildPartialSettingsTree(const TArray<FProperty*>& PropertyChain,
												 const TSharedPtr<FJsonValue>& LeafValue)
{
	TSharedPtr<FJsonValue> WrappedValue = LeafValue;
	for (int32 Index = PropertyChain.Num() - 1; Index >= 0; --Index)
	{
		TSharedPtr<FJsonObject> Object = MakeShared<FJsonObject>();
		Object->SetField(PropertyChain[Index]->GetName(), WrappedValue);
		WrappedValue = MakeShared<FJsonValueObject>(Object);
	}
	return WrappedValue.IsValid() ? WrappedValue->AsObject() : nullptr;
}

bool CollectSettingsWriteUnits(UStruct* Struct, const TSharedPtr<FJsonObject>& Tree,
								TArray<FProperty*>& PropertyChain, TArray<FSettingsWriteUnit>& OutUnits,
								FString& OutError)
{
	if (!Struct || !Tree.IsValid())
	{
		OutError = TEXT("properties must be a valid object");
		return false;
	}

	TArray<TPair<FString, TSharedPtr<FJsonValue>>> SortedFields;
	SortedFields.Reserve(Tree->Values.Num());
	for (const auto& Pair : Tree->Values)
	{
#if UE_VERSION_NEWER_THAN_OR_EQUAL(5, 8, 0)
		SortedFields.Emplace(FString(Pair.Key.ToView()), Pair.Value);
#else
		SortedFields.Emplace(Pair.Key, Pair.Value);
#endif
	}
	SortedFields.Sort([](const auto& A, const auto& B) { return A.Key < B.Key; });

	for (const TPair<FString, TSharedPtr<FJsonValue>>& Pair : SortedFields)
	{
		FProperty* Property = FMonolithReflectionWalker::FindPropertyForwarding(Struct, Pair.Key);
		if (!Property)
		{
			OutError = FString::Printf(TEXT("Unknown settings property '%s' on %s"), *Pair.Key, *Struct->GetName());
			return false;
		}
		if (!IsEditableSettingsProperty(Property))
		{
			OutError = FString::Printf(TEXT("Settings property '%s' on %s is not safely editable"),
				*Property->GetName(), *Struct->GetName());
			return false;
		}

		PropertyChain.Add(Property);
		const FStructProperty* StructProperty = CastField<FStructProperty>(Property);
		const TSharedPtr<FJsonObject> NestedObject =
			StructProperty && Pair.Value.IsValid() && Pair.Value->Type == EJson::Object ? Pair.Value->AsObject() : nullptr;
		if (NestedObject.IsValid() && !NestedObject->Values.IsEmpty())
		{
			if (!CollectSettingsWriteUnits(StructProperty->Struct, NestedObject, PropertyChain, OutUnits, OutError))
			{
				PropertyChain.Pop();
				return false;
			}
		}
		else
		{
			FSettingsWriteUnit& Unit = OutUnits.AddDefaulted_GetRef();
			Unit.PropertyChain = PropertyChain;
			Unit.Tree = BuildPartialSettingsTree(PropertyChain, Pair.Value);
			TArray<FString> PathSegments;
			PathSegments.Reserve(PropertyChain.Num());
			for (const FProperty* PathProperty : PropertyChain)
			{
				PathSegments.Add(PathProperty->GetName());
			}
			Unit.Path = FString::Join(PathSegments, TEXT("."));
		}
		PropertyChain.Pop();
	}
	return true;
}

bool ValidateEditableTree(const UPCGSettings* Settings, const TSharedPtr<FJsonObject>& Tree, FString& OutError,
						  TArray<FSettingsWriteUnit>* OutUnits = nullptr)
{
	if (!Settings || !Tree.IsValid() || Tree->Values.IsEmpty())
	{
		OutError = TEXT("properties must be a non-empty object");
		return false;
	}

	TArray<FSettingsWriteUnit> Units;
	TArray<FProperty*> PropertyChain;
	if (!CollectSettingsWriteUnits(Settings->GetClass(), Tree, PropertyChain, Units, OutError) || Units.IsEmpty())
	{
		if (OutError.IsEmpty())
		{
			OutError = TEXT("properties did not contain any writable leaf values");
		}
		return false;
	}
	Units.Sort([](const FSettingsWriteUnit& A, const FSettingsWriteUnit& B) { return A.Path < B.Path; });

	for (const FSettingsWriteUnit& Unit : Units)
	{
		if (Unit.PropertyChain.IsEmpty() || !CanEditSettingsProperty(Settings, Unit.PropertyChain[0]))
		{
			OutError = FString::Printf(TEXT("Settings property '%s' is disabled for this PCG settings instance"),
				Unit.PropertyChain.IsEmpty() ? TEXT("<unknown>") : *Unit.PropertyChain[0]->GetName());
			return false;
		}
		FEditPropertyChain EditChain = MakeEditPropertyChain(Unit.PropertyChain);
		if (!CanEditSettingsPropertyChain(Settings, EditChain))
		{
			OutError = FString::Printf(TEXT("Settings property path ending in '%s' is disabled for this PCG settings instance"),
				*Unit.PropertyChain.Last()->GetName());
			return false;
		}
	}

	if (OutUnits)
	{
		*OutUnits = MoveTemp(Units);
	}
	return true;
}

class FSettingsPropertySnapshot
{
  public:
	FSettingsPropertySnapshot(UPCGSettings* Settings, FProperty* InProperty) : Property(InProperty)
	{
		if (!Settings || !Property)
		{
			return;
		}
		Value = FMemory::Malloc(Property->GetSize(), FMath::Max(1, Property->GetMinAlignment()));
		Property->InitializeValue(Value);
		Property->CopyCompleteValue(Value, Property->ContainerPtrToValuePtr<void>(Settings));
	}

	~FSettingsPropertySnapshot()
	{
		if (Value && Property)
		{
			Property->DestroyValue(Value);
			FMemory::Free(Value);
		}
	}

	FSettingsPropertySnapshot(const FSettingsPropertySnapshot&) = delete;
	FSettingsPropertySnapshot& operator=(const FSettingsPropertySnapshot&) = delete;

	FProperty* GetProperty() const { return Property; }
	bool IsValid() const { return Property && Value; }

	void PreRestore(UPCGSettings* Settings) const
	{
		if (!Settings || !IsValid())
		{
			return;
		}
		TArray<FProperty*> Properties = {Property};
		FEditPropertyChain Chain = MakeEditPropertyChain(Properties);
		Settings->PreEditChange(Chain);
	}

	void RestoreValue(UPCGSettings* Settings) const
	{
		if (Settings && IsValid())
		{
			Property->CopyCompleteValue(Property->ContainerPtrToValuePtr<void>(Settings), Value);
		}
	}

	void PostRestore(UPCGSettings* Settings) const
	{
		if (!Settings || !IsValid())
		{
			return;
		}
		TArray<FProperty*> Properties = {Property};
		FEditPropertyChain Chain = MakeEditPropertyChain(Properties);
		FPropertyChangedEvent PropertyEvent(Property, EPropertyChangeType::ValueSet);
		FPropertyChangedChainEvent ChainEvent(Chain, PropertyEvent);
		Settings->PostEditChangeChainProperty(ChainEvent);
	}

	bool Matches(const UPCGSettings* Settings) const
	{
		return Settings && IsValid() &&
			Property->Identical(Property->ContainerPtrToValuePtr<void>(Settings), Value, PPF_None);
	}

  private:
	FProperty* Property = nullptr;
	void* Value = nullptr;
};

using FSettingsSnapshots = TArray<TUniquePtr<FSettingsPropertySnapshot>>;

bool CaptureSettingsSnapshots(UPCGSettings* Settings, const TSharedPtr<FJsonObject>& Tree,
							  FSettingsSnapshots& OutSnapshots, FString& OutError)
{
	OutSnapshots.Reset();
	if (!Settings || !Tree.IsValid())
	{
		OutError = TEXT("Cannot snapshot null PCG settings or property tree");
		return false;
	}

	TSet<FProperty*> SeenProperties;
	TArray<FProperty*> SortedProperties;
	for (const auto& Pair : FMonolithJsonUtils::GetFields(Tree))
	{
		FProperty* Property = FMonolithReflectionWalker::FindPropertyForwarding(Settings->GetClass(), Pair.Key);
		if (!Property || SeenProperties.Contains(Property))
		{
			continue;
		}
		SeenProperties.Add(Property);
		SortedProperties.Add(Property);
	}
	SortedProperties.Sort([](const FProperty& A, const FProperty& B) { return A.GetName() < B.GetName(); });
	for (FProperty* Property : SortedProperties)
	{
		TUniquePtr<FSettingsPropertySnapshot> Snapshot = MakeUnique<FSettingsPropertySnapshot>(Settings, Property);
		if (!Snapshot->IsValid())
		{
			OutError = FString::Printf(TEXT("Could not snapshot PCG settings property '%s'"), *Property->GetName());
			return false;
		}
		OutSnapshots.Add(MoveTemp(Snapshot));
	}
	return !OutSnapshots.IsEmpty();
}

bool RestoreSettingsSnapshots(UPCGSettings* Settings, const FSettingsSnapshots& Snapshots)
{
	for (const TUniquePtr<FSettingsPropertySnapshot>& Snapshot : Snapshots)
	{
		if (Snapshot)
		{
			Snapshot->PreRestore(Settings);
		}
	}
	for (const TUniquePtr<FSettingsPropertySnapshot>& Snapshot : Snapshots)
	{
		if (Snapshot)
		{
			Snapshot->RestoreValue(Settings);
		}
	}
	for (const TUniquePtr<FSettingsPropertySnapshot>& Snapshot : Snapshots)
	{
		if (Snapshot)
		{
			Snapshot->PostRestore(Settings);
		}
	}
	for (const TUniquePtr<FSettingsPropertySnapshot>& Snapshot : Snapshots)
	{
		if (!Snapshot || !Snapshot->Matches(Settings))
		{
			return false;
		}
	}
	return true;
}

void MergeDryRunReport(FDryRunReport& Into, const FDryRunReport& From)
{
	for (const FString& Value : From.WouldCreate)
	{
		Into.WouldCreate.AddUnique(Value);
	}
	for (const FString& Value : From.WouldModify)
	{
		Into.WouldModify.AddUnique(Value);
	}
	Into.FieldWrites.Append(From.FieldWrites);
	Into.SilentDrops.Append(From.SilentDrops);
	Into.Errors += From.Errors;
	Into.bWouldApply = Into.bWouldApply && From.bWouldApply;
}

FDryRunReport ApplySettingsTree(UPCGSettings* Settings, const TSharedPtr<FJsonObject>& Tree);

FDryRunReport InspectSettingsTree(const UPCGSettings* Settings, const TSharedPtr<FJsonObject>& Tree)
{
	FDryRunReport Report;
	if (!Settings)
	{
		Report.Errors = 1;
		return Report;
	}

	UPCGSettings* StagedSettings = nullptr;
	UPCGNode* StagedNode = nullptr;
	if (const UPCGNode* SourceNode = Cast<UPCGNode>(Settings->GetOuter()))
	{
		StagedNode = DuplicateObject<UPCGNode>(SourceNode, GetTransientPackage());
		StagedSettings = StagedNode ? StagedNode->GetSettings() : nullptr;
	}
	if (!StagedSettings)
	{
		StagedSettings = DuplicateObject<UPCGSettings>(Settings, GetTransientPackage());
	}
	if (!StagedSettings)
	{
		Report.Errors = 1;
		return Report;
	}

	Report = ApplySettingsTree(StagedSettings, Tree);
	Report.WouldModify.Reset();
	Report.WouldModify.Add(Settings->GetPathName());
	Report.bWouldApply = Report.Errors == 0 && Report.SilentDrops.IsEmpty();
	return Report;
}

FDryRunReport ApplySettingsTree(UPCGSettings* Settings, const TSharedPtr<FJsonObject>& Tree)
{
	FDryRunReport Report;
	Report.bWouldApply = true;
	FString ValidationError;
	TArray<FSettingsWriteUnit> Units;
	if (!ValidateEditableTree(Settings, Tree, ValidationError, &Units))
	{
		Report.Errors = 1;
		Report.bWouldApply = false;
		return Report;
	}

	Settings->Modify();
	for (const FSettingsWriteUnit& Unit : Units)
	{
		FEditPropertyChain Chain = MakeEditPropertyChain(Unit.PropertyChain);
		if (!CanEditSettingsProperty(Settings, Unit.PropertyChain[0]) ||
			!CanEditSettingsPropertyChain(Settings, Chain))
		{
			FBulkFillFieldWrite& FailedWrite = Report.FieldWrites.AddDefaulted_GetRef();
			FailedWrite.Path = Unit.Path;
			FailedWrite.bOk = false;
			FailedWrite.Reason = TEXT("PCG settings callback made this property non-editable before it could be applied");
			++Report.Errors;
			Report.bWouldApply = false;
			break;
		}
		Settings->PreEditChange(Chain);

		FBulkFillSpec Spec;
		Spec.TargetNamespace = TEXT("pcg");
		Spec.TargetAsset = Settings->GetPathName();
		Spec.Tree = Unit.Tree;
		Spec.bDryRun = false;
		Spec.bStrict = true;
		const FDryRunReport UnitReport = FMonolithReflectionWalker::WriteTree(
			Unit.Tree, Settings->GetClass(), Settings, Settings, Spec);

		FPropertyChangedEvent PropertyEvent(Unit.PropertyChain.Last(), EPropertyChangeType::ValueSet);
		FPropertyChangedChainEvent ChainEvent(Chain, PropertyEvent);
		Settings->PostEditChangeChainProperty(ChainEvent);
		MergeDryRunReport(Report, UnitReport);
	}
	Report.WouldModify.AddUnique(Settings->GetPathName());
	Report.bWouldApply = Report.bWouldApply && Report.Errors == 0 && Report.SilentDrops.IsEmpty();
	return Report;
}

TSharedPtr<FJsonObject> BuildGraphValidationReport(UPCGGraph* Graph, const FString& ObjectPath,
											   bool bRequireOutputConnection, bool bRequireNoIsolatedNodes,
											   int32 IssueLimit = MaxValidationIssues)
{
	TArray<FString> Errors;
	TArray<FString> Warnings;
	int32 ErrorCount = 0;
	int32 WarningCount = 0;
	IssueLimit = FMath::Clamp(IssueLimit, 1, MaxValidationIssues);
	auto AddError = [&Errors, &ErrorCount, IssueLimit](FString Message)
	{
		++ErrorCount;
		if (Errors.Num() < IssueLimit)
		{
			Errors.Add(MoveTemp(Message));
		}
	};
	auto AddWarning = [&Warnings, &WarningCount, IssueLimit](FString Message)
	{
		++WarningCount;
		if (Warnings.Num() < IssueLimit)
		{
			Warnings.Add(MoveTemp(Message));
		}
	};
	TSet<FString> NodeIds;
	int32 IsolatedNodeCount = 0;

	if (!Graph)
	{
		AddError(TEXT("PCG graph is null"));
	}
	else
	{
		const UPCGNode* InputNode = Graph->GetInputNode();
		const UPCGNode* OutputNode = Graph->GetOutputNode();
		if (!InputNode || !IsGraphNode(Graph, InputNode))
		{
			AddError(TEXT("Graph input node is missing or does not belong to the graph"));
		}
		if (!OutputNode || !IsGraphNode(Graph, OutputNode))
		{
			AddError(TEXT("Graph output node is missing or does not belong to the graph"));
		}

		for (UPCGNode* Node : Graph->GetNodes())
		{
			if (!Node)
			{
				AddError(TEXT("Graph contains a null element node"));
				continue;
			}
			if (!IsGraphNode(Graph, Node))
			{
				AddError(FString::Printf(TEXT("Node '%s' does not belong to the graph"), *Node->GetName()));
			}

			const FString Id = NodeId(Graph, Node);
			if (NodeIds.Contains(Id))
			{
				AddError(FString::Printf(TEXT("Duplicate node_id '%s'"), *Id));
			}
			NodeIds.Add(Id);
			if (!Node->GetSettings())
			{
				AddError(FString::Printf(TEXT("Node '%s' has no settings object"), *Id));
			}
			if (!Node->HasInboundEdges())
			{
				bool bHasAnyOutputEdge = false;
				for (const UPCGPin* OutputPin : Node->GetOutputPins())
				{
					bHasAnyOutputEdge |= OutputPin && OutputPin->IsConnected();
				}
				if (!bHasAnyOutputEdge)
				{
					++IsolatedNodeCount;
					const FString Message = FString::Printf(TEXT("Node '%s' is isolated"), *Id);
					if (bRequireNoIsolatedNodes)
					{
						AddError(Message);
					}
					else
					{
						AddWarning(Message);
					}
				}
			}
		}
	}

	const TArray<UPCGEdge*> Edges = GetGraphEdges(Graph);
	TSet<FString> EdgeKeys;
	TSet<const UPCGPin*> CapacityCheckedPins;
	TMap<const UPCGNode*, int32> InDegrees;
	TMap<const UPCGNode*, TArray<const UPCGNode*>> DownstreamNodes;
	auto AddTopologyNode = [&InDegrees](const UPCGNode* Node)
	{
		if (Node)
		{
			InDegrees.FindOrAdd(Node, 0);
		}
	};
	if (Graph)
	{
		AddTopologyNode(Graph->GetInputNode());
		AddTopologyNode(Graph->GetOutputNode());
		for (const UPCGNode* Node : Graph->GetNodes())
		{
			AddTopologyNode(Node);
		}
	}
	int32 InvalidEdgeCount = 0;
	for (int32 Index = 0; Index < Edges.Num(); ++Index)
	{
		const UPCGEdge* Edge = Edges[Index];
		const UPCGPin* SourcePin = Edge ? Edge->InputPin.Get() : nullptr;
		const UPCGPin* TargetPin = Edge ? Edge->OutputPin.Get() : nullptr;
		const UPCGNode* SourceNode = SourcePin ? SourcePin->Node.Get() : nullptr;
		const UPCGNode* TargetNode = TargetPin ? TargetPin->Node.Get() : nullptr;
		const bool bEndpointPinsOwned =
			SourceNode && TargetNode && SourcePin && TargetPin &&
			SourceNode->GetOutputPin(SourcePin->Properties.Label) == SourcePin &&
			TargetNode->GetInputPin(TargetPin->Properties.Label) == TargetPin;
		auto ContainsEdge = [Edge](const UPCGPin* Pin)
		{
			if (!Pin || !Edge)
			{
				return false;
			}
			for (const UPCGEdge* AttachedEdge : Pin->Edges)
			{
				if (AttachedEdge == Edge)
				{
					return true;
				}
			}
			return false;
		};
		const bool bAttachedAtBothEndpoints = ContainsEdge(SourcePin) && ContainsEdge(TargetPin);
		FString EdgeError;
		if (!Edge || !Edge->IsValid() || !SourcePin || !TargetPin)
		{
			EdgeError = TEXT("edge object or endpoint pin is invalid");
		}
		else if (!IsGraphNode(Graph, SourceNode) || !IsGraphNode(Graph, TargetNode))
		{
			EdgeError = TEXT("edge endpoint does not belong to the graph");
		}
		else if (!SourcePin->IsOutputPin() || TargetPin->IsOutputPin())
		{
			EdgeError = TEXT("edge direction is not output-to-input");
		}
		else if (!bEndpointPinsOwned)
		{
			EdgeError = TEXT("edge endpoint pin is not owned by the endpoint node's current pin array");
		}
		else if (!bAttachedAtBothEndpoints)
		{
			EdgeError = TEXT("edge is not attached to both endpoint pin edge arrays");
		}
		else if (SourcePin->GetCompatibilityWithOtherPin(TargetPin) != EPCGDataTypeCompatibilityResult::Compatible)
		{
			EdgeError = TEXT("edge pins are not directly compatible without a filter or conversion node");
		}
		else
		{
			const FString EdgeKey = NodeId(Graph, SourceNode) + TEXT("|") +
				SourcePin->Properties.Label.ToString() + TEXT("|") + NodeId(Graph, TargetNode) + TEXT("|") +
				TargetPin->Properties.Label.ToString();
			if (EdgeKeys.Contains(EdgeKey))
			{
				EdgeError = TEXT("duplicate edge");
			}
			EdgeKeys.Add(EdgeKey);
		}

		if (SourceNode && TargetNode && IsGraphNode(Graph, SourceNode) && IsGraphNode(Graph, TargetNode) &&
			SourcePin && TargetPin && SourcePin->IsOutputPin() && !TargetPin->IsOutputPin() &&
			bEndpointPinsOwned && bAttachedAtBothEndpoints)
		{
			DownstreamNodes.FindOrAdd(SourceNode).Add(TargetNode);
			++InDegrees.FindOrAdd(TargetNode, 0);
		}

		if (!EdgeError.IsEmpty())
		{
			++InvalidEdgeCount;
			AddError(FString::Printf(TEXT("Edge %d: %s"), Index, *EdgeError));
		}

		if (TargetPin && !CapacityCheckedPins.Contains(TargetPin))
		{
			CapacityCheckedPins.Add(TargetPin);
			if (!TargetPin->AllowsMultipleConnections() && TargetPin->EdgeCount() > 1)
			{
				++InvalidEdgeCount;
				AddError(FString::Printf(TEXT("Input pin '%s' on node '%s' has %d edges but supports only one"),
					*TargetPin->Properties.Label.ToString(), *NodeId(Graph, TargetNode), TargetPin->EdgeCount()));
			}
		}
	}

	TArray<const UPCGNode*> ZeroInDegreeNodes;
	ZeroInDegreeNodes.Reserve(InDegrees.Num());
	for (const TPair<const UPCGNode*, int32>& Pair : InDegrees)
	{
		if (Pair.Value == 0)
		{
			ZeroInDegreeNodes.Add(Pair.Key);
		}
	}
	int32 ProcessedTopologyNodes = 0;
	while (!ZeroInDegreeNodes.IsEmpty())
	{
		const UPCGNode* Node = ZeroInDegreeNodes.Pop(EAllowShrinking::No);
		++ProcessedTopologyNodes;
		if (const TArray<const UPCGNode*>* Targets = DownstreamNodes.Find(Node))
		{
			for (const UPCGNode* Target : *Targets)
			{
				int32* InDegree = InDegrees.Find(Target);
				if (InDegree && --(*InDegree) == 0)
				{
					ZeroInDegreeNodes.Add(Target);
				}
			}
		}
	}
	const bool bHasCycle = ProcessedTopologyNodes != InDegrees.Num();
	if (bHasCycle)
	{
		++InvalidEdgeCount;
		AddError(TEXT("Graph contains a directed cycle"));
	}

	const bool bOutputConnected = Graph && Graph->GetOutputNode() && Graph->GetOutputNode()->HasInboundEdges();
	if (bRequireOutputConnection && !bOutputConnected)
	{
		AddError(TEXT("Graph output node has no inbound edge"));
	}

	TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
	Result->SetStringField(TEXT("namespace"), TEXT("pcg"));
	Result->SetStringField(TEXT("action"), TEXT("validate_pcg_graph"));
	Result->SetStringField(TEXT("asset_path"), ObjectPath);
	Result->SetBoolField(TEXT("valid"), ErrorCount == 0);
	Result->SetBoolField(TEXT("output_connected"), bOutputConnected);
	Result->SetBoolField(TEXT("require_output_connection"), bRequireOutputConnection);
	Result->SetBoolField(TEXT("require_no_isolated_nodes"), bRequireNoIsolatedNodes);
	Result->SetNumberField(TEXT("element_node_count"), Graph ? Graph->GetNodes().Num() : 0);
	Result->SetNumberField(TEXT("edge_count"), Edges.Num());
	Result->SetNumberField(TEXT("invalid_edge_count"), InvalidEdgeCount);
	Result->SetBoolField(TEXT("has_cycle"), bHasCycle);
	Result->SetNumberField(TEXT("isolated_node_count"), IsolatedNodeCount);
	Result->SetNumberField(TEXT("error_count"), ErrorCount);
	Result->SetNumberField(TEXT("warning_count"), WarningCount);
	Result->SetNumberField(TEXT("returned_error_count"), Errors.Num());
	Result->SetNumberField(TEXT("returned_warning_count"), Warnings.Num());
	Result->SetBoolField(TEXT("errors_truncated"), ErrorCount > Errors.Num());
	Result->SetBoolField(TEXT("warnings_truncated"), WarningCount > Warnings.Num());
	Result->SetNumberField(TEXT("issue_limit"), IssueLimit);
	Result->SetArrayField(TEXT("errors"), StringsToJson(Errors));
	Result->SetArrayField(TEXT("warnings"), StringsToJson(Warnings));
	return Result;
}

bool ValidateGraphForCommit(UPCGGraph* Graph, const FString& ObjectPath, TSharedPtr<FJsonObject>& OutReport,
							FString& OutError)
{
	OutReport = BuildGraphValidationReport(Graph, ObjectPath, false, false, 64);
	bool bValid = false;
	if (!OutReport.IsValid() || !OutReport->TryGetBoolField(TEXT("valid"), bValid) || !bValid)
	{
		double ErrorCount = 0.0;
		if (OutReport.IsValid())
		{
			OutReport->TryGetNumberField(TEXT("error_count"), ErrorCount);
		}
		OutError = FString::Printf(TEXT("PCG graph failed pre-save structural validation with %d error(s)"),
			static_cast<int32>(ErrorCount));
		return false;
	}
	return true;
}

void RestorePackageDirtyState(UPCGGraph* Graph, bool bWasDirty)
{
	if (Graph && Graph->GetPackage())
	{
		Graph->GetPackage()->SetDirtyFlag(bWasDirty);
	}
}

struct FGraphEdgeDescriptor
{
	UPCGNode* SourceNode = nullptr;
	FName SourcePin;
	UPCGNode* TargetNode = nullptr;
	FName TargetPin;
};

TArray<FGraphEdgeDescriptor> CaptureIncidentEdges(UPCGGraph* Graph, const UPCGNode* Node)
{
	TArray<FGraphEdgeDescriptor> Descriptors;
	for (UPCGEdge* Edge : GetGraphEdges(Graph))
	{
		const UPCGPin* SourcePin = Edge ? Edge->InputPin.Get() : nullptr;
		const UPCGPin* TargetPin = Edge ? Edge->OutputPin.Get() : nullptr;
		UPCGNode* SourceNode = SourcePin ? SourcePin->Node.Get() : nullptr;
		UPCGNode* TargetNode = TargetPin ? TargetPin->Node.Get() : nullptr;
		if (SourceNode == Node || TargetNode == Node)
		{
			FGraphEdgeDescriptor& Descriptor = Descriptors.AddDefaulted_GetRef();
			Descriptor.SourceNode = SourceNode;
			Descriptor.SourcePin = SourcePin ? SourcePin->Properties.Label : NAME_None;
			Descriptor.TargetNode = TargetNode;
			Descriptor.TargetPin = TargetPin ? TargetPin->Properties.Label : NAME_None;
		}
	}
	return Descriptors;
}

bool RestoreRemovedNode(UPCGGraph* Graph, UPCGNode* Node, const FName OriginalNodeName,
						const TArray<FGraphEdgeDescriptor>& IncidentEdges)
{
	if (!Graph || !Node)
	{
		return false;
	}

	FMonolithPCGScopedGraphEditNotifications NotificationScope(Graph);
	if (!Graph->Contains(Node))
	{
		Graph->AddNode(Node);
	}
	if (Node->GetFName() != OriginalNodeName && !FindObject<UPCGNode>(Graph, *OriginalNodeName.ToString()))
	{
		Node->Rename(*OriginalNodeName.ToString(), Graph, REN_DontCreateRedirectors | REN_AllowPackageLinkerMismatch);
	}

	bool bRestored = Graph->Contains(Node);
	for (const FGraphEdgeDescriptor& Edge : IncidentEdges)
	{
		if (!Edge.SourceNode || !Edge.TargetNode || Edge.SourcePin.IsNone() || Edge.TargetPin.IsNone())
		{
			bRestored = false;
			continue;
		}
		Graph->AddLabeledEdge(Edge.SourceNode, Edge.SourcePin, Edge.TargetNode, Edge.TargetPin);
		bRestored &= AreConnected(Edge.SourceNode->GetOutputPin(Edge.SourcePin),
			Edge.TargetNode->GetInputPin(Edge.TargetPin));
	}
	NotificationScope.MarkExternalModification();
	return bRestored;
}

bool RollbackAddedNode(UPCGGraph* Graph, UPCGNode* Node)
{
	if (!Graph || !Node)
	{
		return false;
	}
	if (Graph->Contains(Node))
	{
		FMonolithPCGScopedGraphEditNotifications NotificationScope(Graph);
		Graph->RemoveNode(Node);
		NotificationScope.MarkExternalModification();
	}

	// UE 5.8 moves removed PCG nodes to the transient package; UE 5.7
	// leaves them as graph inners. Normalize both versions so a failed add
	// cannot leave a ghost UObject that perturbs future unique names.
	if (Node->GetOuter() == Graph)
	{
		Node->Rename(nullptr, GetTransientPackage(), REN_DontCreateRedirectors | REN_AllowPackageLinkerMismatch);
	}
	const bool bDetached = !Graph->Contains(Node) && Node->GetOuter() != Graph;
	if (bDetached)
	{
		Node->ClearFlags(RF_Public | RF_Standalone);
		Node->MarkAsGarbage();
	}
	return bDetached;
}

void RollbackConnectedEdge(UPCGGraph* Graph, UPCGNode* SourceNode, const FName SourcePin, UPCGNode* TargetNode,
						   const FName TargetPin)
{
	if (!Graph || !SourceNode || !TargetNode)
	{
		return;
	}
	FMonolithPCGScopedGraphEditNotifications NotificationScope(Graph);
	Graph->RemoveEdge(SourceNode, SourcePin, TargetNode, TargetPin);
	NotificationScope.MarkExternalModification();
}

bool RollbackDisconnectedEdge(UPCGGraph* Graph, UPCGNode* SourceNode, const FName SourcePin,
							  UPCGNode* TargetNode, const FName TargetPin)
{
	if (!Graph || !SourceNode || !TargetNode)
	{
		return false;
	}
	FMonolithPCGScopedGraphEditNotifications NotificationScope(Graph);
	Graph->AddLabeledEdge(SourceNode, SourcePin, TargetNode, TargetPin);
	NotificationScope.MarkExternalModification();
	return AreConnected(SourceNode->GetOutputPin(SourcePin), TargetNode->GetInputPin(TargetPin));
}

bool RestoreSettingsMutation(UPCGGraph* Graph, UPCGNode* Node, UPCGSettings* Settings,
							 const FSettingsSnapshots& Snapshots,
							 const TArray<FGraphEdgeDescriptor>& OriginalIncidentEdges, bool bWasDirty)
{
	if (!Graph || !Node || !Settings)
	{
		return false;
	}

	bool bRestored = false;
	{
		FMonolithPCGScopedGraphEditNotifications NotificationScope(Graph);
		bRestored = RestoreSettingsSnapshots(Settings, Snapshots);
		for (const FGraphEdgeDescriptor& CurrentEdge : CaptureIncidentEdges(Graph, Node))
		{
			if (CurrentEdge.SourceNode && CurrentEdge.TargetNode)
			{
				Graph->RemoveEdge(CurrentEdge.SourceNode, CurrentEdge.SourcePin, CurrentEdge.TargetNode,
					CurrentEdge.TargetPin);
			}
		}

		for (const FGraphEdgeDescriptor& OriginalEdge : OriginalIncidentEdges)
		{
			if (!OriginalEdge.SourceNode || !OriginalEdge.TargetNode)
			{
				bRestored = false;
				continue;
			}
			Graph->AddLabeledEdge(OriginalEdge.SourceNode, OriginalEdge.SourcePin, OriginalEdge.TargetNode,
				OriginalEdge.TargetPin);
			bRestored &= AreConnected(OriginalEdge.SourceNode->GetOutputPin(OriginalEdge.SourcePin),
				OriginalEdge.TargetNode->GetInputPin(OriginalEdge.TargetPin));
		}
		NotificationScope.MarkExternalModification();
	}
	RestorePackageDirtyState(Graph, bWasDirty);
	bRestored &= Graph->GetPackage() && Graph->GetPackage()->IsDirty() == bWasDirty;
	return bRestored;
}

void DiscardCreatedGraph(UPCGGraph* Graph)
{
	if (!Graph)
	{
		return;
	}
	UPackage* Package = Graph->GetPackage();
	FAssetRegistryModule::AssetDeleted(Graph);
	Graph->ClearFlags(RF_Public | RF_Standalone);
	Graph->Rename(nullptr, GetTransientPackage(), REN_DontCreateRedirectors | REN_AllowPackageLinkerMismatch);
	Graph->MarkAsGarbage();
	if (Package)
	{
		Package->SetDirtyFlag(false);
	}
}

TSharedPtr<FJsonObject> MutationResult(const TCHAR* Action, const FString& ObjectPath, const FString& Status,
									   bool bSaved, const FString& SavedFilename)
{
	TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
	Result->SetStringField(TEXT("namespace"), TEXT("pcg"));
	Result->SetStringField(TEXT("action"), Action);
	Result->SetStringField(TEXT("asset_path"), ObjectPath);
	Result->SetStringField(TEXT("status"), Status);
	Result->SetBoolField(TEXT("saved"), bSaved);
	if (!SavedFilename.IsEmpty())
	{
		Result->SetStringField(TEXT("saved_filename"), SavedFilename);
	}
	return Result;
}

void AddBoundedPinFields(const TSharedPtr<FJsonObject>& Result, UPCGNode* Node)
{
	if (!Result.IsValid() || !Node)
	{
		return;
	}
	constexpr int32 MutationPinLimit = 128;
	int32 RemainingItems = MutationPinLimit * 2;
	bool bInputPinsTruncated = false;
	bool bOutputPinsTruncated = false;
	Result->SetArrayField(TEXT("input_pins"),
		SerializePins(Node->GetInputPins(), TEXT("input"), MutationPinLimit, RemainingItems, bInputPinsTruncated));
	Result->SetArrayField(TEXT("output_pins"),
		SerializePins(Node->GetOutputPins(), TEXT("output"), MutationPinLimit, RemainingItems, bOutputPinsTruncated));
	Result->SetBoolField(TEXT("input_pins_truncated"), bInputPinsTruncated);
	Result->SetBoolField(TEXT("output_pins_truncated"), bOutputPinsTruncated);
}
} // namespace MonolithPCGAuthoring

void FMonolithPCGGraphAuthoringActions::RegisterActions(FMonolithToolRegistry& Registry)
{
	Registry.RegisterAction(
		TEXT("pcg"), TEXT("list_pcg_node_types"),
		TEXT("List concrete UPCGSettings node classes from the live "
			 "engine/project reflection surface, with stable aliases and "
			 "optional editable property summaries."),
		FMonolithActionHandler::CreateStatic(&ListNodeTypes),
		FParamSchemaBuilder()
			.EnableValidation()
			.Optional(TEXT("query"), TEXT("string"),
					  TEXT("Case-insensitive class, friendly-name, title, or "
						   "path filter"))
			.Optional(TEXT("include_properties"), TEXT("bool"), TEXT("Include bounded editable property descriptors"),
					  TEXT("false"))
			.Optional(TEXT("property_limit"), TEXT("integer"),
					  TEXT("Maximum editable properties per node type (1-256)"), TEXT("64"))
			.Optional(TEXT("limit"), TEXT("integer"), TEXT("Maximum node types to return (1-1000)"), TEXT("200"))
			.Build(),
		TEXT("Graph Authoring"));

	Registry.RegisterAction(
		TEXT("pcg"), TEXT("create_pcg_graph"),
		TEXT("Create a project-owned UPCGGraph asset, or return an existing "
			 "UPCGGraph when existing_policy=return_existing. Save defaults "
			 "true."),
		FMonolithActionHandler::CreateStatic(&CreateGraph),
		FParamSchemaBuilder()
			.EnableValidation()
			.Required(TEXT("asset_path"), TEXT("string"), TEXT("Project-owned PCG graph package path"))
			.Optional(TEXT("existing_policy"), TEXT("string"),
					  TEXT("Existing destination policy: fail or return_existing"), TEXT("fail"))
			.Optional(TEXT("save"), TEXT("bool"), TEXT("Persist the new graph package"), TEXT("true"))
			.Build(),
		TEXT("Graph Authoring"), MonolithPCGAuthoring::MutationPolicy());

	Registry.RegisterAction(
		TEXT("pcg"), TEXT("get_pcg_graph_info"),
		TEXT("Read a PCG graph's element nodes, special input/output nodes, "
			 "pins, edges, positions, settings classes, and optional bounded "
			 "settings values."),
		FMonolithActionHandler::CreateStatic(&GetGraphInfo),
		FParamSchemaBuilder()
			.EnableValidation()
			.Required(TEXT("asset_path"), TEXT("string"), TEXT("Project-owned PCG graph package or object path"))
			.Optional(TEXT("include_settings"), TEXT("bool"), TEXT("Include bounded editable settings values"),
					  TEXT("false"))
			.Optional(TEXT("settings_fields"), TEXT("array"), TEXT("Optional settings property-name allowlist"))
			.Optional(TEXT("property_limit"), TEXT("integer"), TEXT("Maximum properties per settings object (1-256)"),
					  TEXT("64"))
			.Optional(TEXT("array_limit"), TEXT("integer"), TEXT("Maximum values per nested array/map (1-256)"),
					  TEXT("32"))
			.Optional(TEXT("node_limit"), TEXT("integer"), TEXT("Maximum element nodes to return (1-5000)"),
					  TEXT("500"))
			.Optional(TEXT("edge_limit"), TEXT("integer"), TEXT("Maximum graph edges to return (1-20000)"),
					  TEXT("2000"))
			.Optional(TEXT("pin_limit"), TEXT("integer"),
					  TEXT("Maximum input or output pins returned per node (1-1024)"), TEXT("128"))
			.Optional(TEXT("response_item_limit"), TEXT("integer"),
					  TEXT("Shared budget for returned nodes, pins, edges, and nested settings values (1-100000)"),
					  TEXT("20000"))
			.Build(),
		TEXT("Graph Authoring"));

	Registry.RegisterAction(
		TEXT("pcg"), TEXT("add_pcg_node"),
		TEXT("Add one reflected UPCGSettings node to an existing PCG graph, or "
			 "return the same typed node by authored title when "
			 "existing_policy=return_existing."),
		FMonolithActionHandler::CreateStatic(&AddNode),
		FParamSchemaBuilder()
			.EnableValidation()
			.Required(TEXT("asset_path"), TEXT("string"), TEXT("Project-owned PCG graph package or object path"))
			.Required(TEXT("node_type"), TEXT("string"),
					  TEXT("Friendly, exact, or /Script UPCGSettings class identifier"))
			.Optional(TEXT("node_title"), TEXT("string"), TEXT("Authored node title; must be unique when supplied"))
			.Optional(TEXT("existing_policy"), TEXT("string"),
					  TEXT("Authored-title conflict policy: fail or return_existing"), TEXT("fail"))
			.Optional(TEXT("position"), TEXT("array"), TEXT("Editor graph position [x,y]"))
			.Optional(TEXT("properties"), TEXT("object"), TEXT("Validated initial UPCGSettings property tree"))
			.Optional(TEXT("save"), TEXT("bool"), TEXT("Persist the graph after mutation"), TEXT("true"))
			.Build(),
		TEXT("Graph Authoring"), MonolithPCGAuthoring::MutationPolicy());

	Registry.RegisterAction(
		TEXT("pcg"), TEXT("remove_pcg_node"),
		TEXT("Remove one element node and all of its incident edges from a PCG "
			 "graph. Graph input/output nodes are protected."),
		FMonolithActionHandler::CreateStatic(&RemoveNode),
		FParamSchemaBuilder()
			.EnableValidation()
			.Required(TEXT("asset_path"), TEXT("string"), TEXT("Project-owned PCG graph package or object path"))
			.Required(TEXT("node_id"), TEXT("string"),
					  TEXT("Exact node_id returned by add/get, or a unique "
						   "authored title"))
			.Optional(TEXT("save"), TEXT("bool"), TEXT("Persist the graph after mutation"), TEXT("true"))
			.Build(),
		TEXT("Graph Authoring"), MonolithPCGAuthoring::MutationPolicy());

	Registry.RegisterAction(
		TEXT("pcg"), TEXT("connect_pcg_nodes"),
		TEXT("Connect an existing source output pin to a target input pin after "
			 "graph ownership, direction, direct PCG data-type compatibility, "
			 "acyclicity, and input-capacity checks; repeated calls are "
			 "idempotent."),
		FMonolithActionHandler::CreateStatic(&ConnectNodes),
		FParamSchemaBuilder()
			.EnableValidation()
			.Required(TEXT("asset_path"), TEXT("string"), TEXT("Project-owned PCG graph package or object path"))
			.Required(TEXT("source_node"), TEXT("string"),
					  TEXT("Source node_id; __input__ addresses the graph input node"))
			.Required(TEXT("source_pin"), TEXT("string"), TEXT("Source output pin label"))
			.Required(TEXT("target_node"), TEXT("string"),
					  TEXT("Target node_id; __output__ addresses the graph output node"))
			.Required(TEXT("target_pin"), TEXT("string"), TEXT("Target input pin label"))
			.Optional(TEXT("save"), TEXT("bool"), TEXT("Persist the graph after mutation"), TEXT("true"))
			.Build(),
		TEXT("Graph Authoring"), MonolithPCGAuthoring::MutationPolicy());

	Registry.RegisterAction(
		TEXT("pcg"), TEXT("disconnect_pcg_nodes"),
		TEXT("Disconnect one exact PCG edge. A missing edge is an idempotent "
			 "success and is reported as not_connected."),
		FMonolithActionHandler::CreateStatic(&DisconnectNodes),
		FParamSchemaBuilder()
			.EnableValidation()
			.Required(TEXT("asset_path"), TEXT("string"), TEXT("Project-owned PCG graph package or object path"))
			.Required(TEXT("source_node"), TEXT("string"),
					  TEXT("Source node_id; __input__ addresses the graph input node"))
			.Required(TEXT("source_pin"), TEXT("string"), TEXT("Source output pin label"))
			.Required(TEXT("target_node"), TEXT("string"),
					  TEXT("Target node_id; __output__ addresses the graph output node"))
			.Required(TEXT("target_pin"), TEXT("string"), TEXT("Target input pin label"))
			.Optional(TEXT("save"), TEXT("bool"), TEXT("Persist the graph after mutation"), TEXT("true"))
			.Build(),
		TEXT("Graph Authoring"), MonolithPCGAuthoring::MutationPolicy());

	Registry.RegisterAction(
		TEXT("pcg"), TEXT("set_pcg_node_params"),
		TEXT("Validate and apply a strict JSON property tree to one UPCGSettings "
			 "object through Monolith's canonical reflection walker and "
			 "property-chain editor callbacks. Supports side-effect-free staged "
			 "dry-run."),
		FMonolithActionHandler::CreateStatic(&SetNodeParams),
		FParamSchemaBuilder()
			.EnableValidation()
			.Required(TEXT("asset_path"), TEXT("string"), TEXT("Project-owned PCG graph package or object path"))
			.Required(TEXT("node_id"), TEXT("string"),
					  TEXT("Exact node_id returned by add/get, or a unique "
						   "authored title"))
			.Required(TEXT("properties"), TEXT("object"), TEXT("Editable UPCGSettings property tree"))
			.Optional(TEXT("dry_run"), TEXT("bool"), TEXT("Validate and report without mutation"), TEXT("false"))
			.Optional(TEXT("save"), TEXT("bool"), TEXT("Persist the graph after mutation"), TEXT("true"))
			.Build(),
		TEXT("Graph Authoring"), MonolithPCGAuthoring::MutationPolicy());

	Registry.RegisterAction(
		TEXT("pcg"), TEXT("validate_pcg_graph"),
		TEXT("Validate PCG graph node ownership, settings, edge "
			 "endpoints/directions/types/capacity, duplicate ids/edges, directed "
			 "cycles, isolated nodes, and optional output connectivity without "
			 "mutation."),
		FMonolithActionHandler::CreateStatic(&ValidateGraph),
		FParamSchemaBuilder()
			.EnableValidation()
			.Required(TEXT("asset_path"), TEXT("string"), TEXT("Project-owned PCG graph package or object path"))
			.Optional(TEXT("require_output_connection"), TEXT("bool"),
					  TEXT("Treat an unconnected graph output as an error"), TEXT("false"))
			.Optional(TEXT("require_no_isolated_nodes"), TEXT("bool"),
					  TEXT("Treat isolated element nodes as errors instead of warnings"), TEXT("false"))
			.Optional(TEXT("issue_limit"), TEXT("integer"),
					  TEXT("Maximum errors and warnings returned per array (1-1000)"), TEXT("200"))
			.Build(),
		TEXT("Graph Authoring"));

	const TArray<FString> AuthoringActions = {
		TEXT("list_pcg_node_types"),  TEXT("create_pcg_graph"),	   TEXT("get_pcg_graph_info"),
		TEXT("add_pcg_node"),		  TEXT("remove_pcg_node"),	   TEXT("connect_pcg_nodes"),
		TEXT("disconnect_pcg_nodes"), TEXT("set_pcg_node_params"), TEXT("validate_pcg_graph")};
	for (const FString& Action : AuthoringActions)
	{
		Registry.SetActionSearchMetadata(
			TEXT("pcg"), Action,
			{TEXT("PCG graph authoring"), TEXT("procedural content generation asset editing"), TEXT("PCG node graph")},
			{TEXT("edit PCG asset"), TEXT("build PCG graph")}, {});
	}

	Registry.SetActionPlanningMetadata(TEXT("pcg"), TEXT("create_pcg_graph"), TEXT("unreal-pcg"),
									   {TEXT("The PCG plugin must be enabled and the destination must not "
											 "already exist")},
									   {TEXT("Created graph object path, package path, save status, and initial "
											 "node/edge counts")},
									   {TEXT("pcg.add_pcg_node"), TEXT("pcg.get_pcg_graph_info")});
	Registry.SetActionPlanningMetadata(TEXT("pcg"), TEXT("set_pcg_node_params"), TEXT("unreal-pcg"),
									   {TEXT("Use pcg.list_pcg_node_types(include_properties=true) or "
											 "dry_run=true before unfamiliar settings writes")},
									   {TEXT("Per-field canonical reflection-walker report plus save status")},
									   {TEXT("pcg.get_pcg_graph_info"), TEXT("pcg.validate_pcg_graph")});
	Registry.SetActionPlanningMetadata(TEXT("pcg"), TEXT("validate_pcg_graph"), TEXT("unreal-pcg"), {},
									   {TEXT("Bounded structural validation report with valid, errors, "
											 "warnings, node_count, and edge_count")},
									   {TEXT("pcg.get_pcg_graph_info")});
}

FMonolithActionResult FMonolithPCGGraphAuthoringActions::ListNodeTypes(const TSharedPtr<FJsonObject>& Params)
{
	FString Query;
	if (Params.IsValid() && Params->HasField(TEXT("query")) && !Params->TryGetStringField(TEXT("query"), Query))
	{
		return MonolithPCGAuthoring::InvalidParam(TEXT("query"), TEXT("query must be a string"));
	}
	Query.TrimStartAndEndInline();

	bool bIncludeProperties = false;
	int32 PropertyLimit = 64;
	int32 Limit = 200;
	FString Error;
	if (!MonolithPCGAuthoring::ReadOptionalBool(Params, TEXT("include_properties"), false, bIncludeProperties, Error) ||
		!MonolithPCGAuthoring::ReadOptionalInt(Params, TEXT("property_limit"), 64, 1, 256, PropertyLimit, Error) ||
		!MonolithPCGAuthoring::ReadOptionalInt(Params, TEXT("limit"), 200, 1, MonolithPCGAuthoring::MaxNodeTypes, Limit,
											   Error))
	{
		return MonolithPCGAuthoring::InvalidParam(TEXT("params"), Error);
	}

	const TArray<FMonolithPCGSettingsTypeInfo> Types = FMonolithPCGSettingsResolver::ListTypes();
	TArray<TSharedPtr<FJsonValue>> Rows;
	int32 MatchedCount = 0;
	for (const FMonolithPCGSettingsTypeInfo& Type : Types)
	{
		const bool bMatches = Query.IsEmpty() || Type.ClassName.Contains(Query, ESearchCase::IgnoreCase) ||
							  Type.ClassPath.Contains(Query, ESearchCase::IgnoreCase) ||
							  Type.FriendlyName.Contains(Query, ESearchCase::IgnoreCase) ||
							  Type.DefaultNodeTitle.Contains(Query, ESearchCase::IgnoreCase);
		if (!bMatches)
		{
			continue;
		}
		++MatchedCount;
		if (Rows.Num() >= Limit)
		{
			continue;
		}

		TSharedPtr<FJsonObject> Row = MakeShared<FJsonObject>();
		Row->SetStringField(TEXT("friendly_name"), Type.FriendlyName);
		Row->SetStringField(TEXT("class_name"), Type.ClassName);
		Row->SetStringField(TEXT("class_path"), Type.ClassPath);
		Row->SetStringField(TEXT("default_node_title"), Type.DefaultNodeTitle);
		Row->SetBoolField(TEXT("native"), Type.SettingsClass->ClassGeneratedBy == nullptr);

		const UPCGSettings* DefaultSettings = Type.SettingsClass->GetDefaultObject<UPCGSettings>();
		if (DefaultSettings)
		{
			if (const UEnum* SettingsTypeEnum = StaticEnum<EPCGSettingsType>())
			{
				Row->SetStringField(TEXT("settings_type"), SettingsTypeEnum->GetNameStringByValue(
															   static_cast<int64>(DefaultSettings->GetType())));
			}
		}

		if (bIncludeProperties)
		{
			TArray<TSharedPtr<FJsonValue>> PropertyRows;
			int32 EditablePropertyCount = 0;
			for (TFieldIterator<FProperty> It(Type.SettingsClass); It; ++It)
			{
				FProperty* Property = *It;
				if (!MonolithPCGAuthoring::IsEditableSettingsProperty(Property) || !DefaultSettings ||
					!MonolithPCGAuthoring::CanEditSettingsProperty(DefaultSettings, Property))
				{
					continue;
				}
				++EditablePropertyCount;
				if (PropertyRows.Num() >= PropertyLimit)
				{
					continue;
				}
				TSharedPtr<FJsonObject> PropertyRow = MakeShared<FJsonObject>();
				PropertyRow->SetStringField(TEXT("name"), Property->GetName());
				PropertyRow->SetStringField(TEXT("type"), Property->GetCPPType());
				PropertyRow->SetStringField(TEXT("category"), Property->GetMetaData(TEXT("Category")));
				PropertyRows.Add(MakeShared<FJsonValueObject>(PropertyRow));
			}
			Row->SetNumberField(TEXT("editable_property_count"), EditablePropertyCount);
			Row->SetBoolField(TEXT("properties_truncated"), EditablePropertyCount > PropertyRows.Num());
			Row->SetArrayField(TEXT("editable_properties"), PropertyRows);
		}
		Rows.Add(MakeShared<FJsonValueObject>(Row));
	}

	TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
	Result->SetStringField(TEXT("namespace"), TEXT("pcg"));
	Result->SetStringField(TEXT("action"), TEXT("list_pcg_node_types"));
	Result->SetStringField(TEXT("query"), Query);
	Result->SetNumberField(TEXT("available_count"), Types.Num());
	Result->SetNumberField(TEXT("matched_count"), MatchedCount);
	Result->SetNumberField(TEXT("returned_count"), Rows.Num());
	Result->SetBoolField(TEXT("truncated"), MatchedCount > Rows.Num());
	Result->SetArrayField(TEXT("node_types"), Rows);
	return FMonolithActionResult::Success(Result);
}

FMonolithActionResult FMonolithPCGGraphAuthoringActions::CreateGraph(const TSharedPtr<FJsonObject>& Params)
{
	FString AssetPath;
	FString Error;
	if (!MonolithPCGAuthoring::ReadRequiredString(Params, TEXT("asset_path"), AssetPath, Error))
	{
		return MonolithPCGAuthoring::InvalidParam(TEXT("asset_path"), Error);
	}

	bool bSave = true;
	if (!MonolithPCGAuthoring::ReadOptionalBool(Params, TEXT("save"), true, bSave, Error))
	{
		return MonolithPCGAuthoring::InvalidParam(TEXT("save"), Error);
	}
	FString ExistingPolicy = TEXT("fail");
	if (Params.IsValid() && Params->HasField(TEXT("existing_policy")) &&
		!Params->TryGetStringField(TEXT("existing_policy"), ExistingPolicy))
	{
		return MonolithPCGAuthoring::InvalidParam(TEXT("existing_policy"), TEXT("existing_policy must be a string"));
	}
	ExistingPolicy.TrimStartAndEndInline();
	if (!ExistingPolicy.Equals(TEXT("fail"), ESearchCase::IgnoreCase) &&
		!ExistingPolicy.Equals(TEXT("return_existing"), ESearchCase::IgnoreCase))
	{
		return MonolithPCGAuthoring::InvalidParam(TEXT("existing_policy"),
												  TEXT("existing_policy must be 'fail' or 'return_existing'"));
	}

	FString PackageName;
	FString ObjectPath;
	if (!MonolithPCGAuthoring::NormalizeGraphPath(AssetPath, PackageName, ObjectPath, Error))
	{
		return MonolithPCGAuthoring::InvalidParam(TEXT("asset_path"), Error);
	}
	if (FMonolithAssetUtils::AssetExists(ObjectPath))
	{
		if (ExistingPolicy.Equals(TEXT("return_existing"), ESearchCase::IgnoreCase))
		{
			UPCGGraph* ExistingGraph = nullptr;
			FString ExistingObjectPath;
			if (!MonolithPCGAuthoring::LoadGraph(ObjectPath, ExistingGraph, ExistingObjectPath, Error))
			{
				return MonolithPCGAuthoring::InvalidParam(TEXT("asset_path"), Error);
			}
			TSharedPtr<FJsonObject> Result = MonolithPCGAuthoring::MutationResult(
				TEXT("create_pcg_graph"), ExistingObjectPath, TEXT("existing"), false, FString());
			Result->SetStringField(TEXT("package_name"), PackageName);
			Result->SetStringField(TEXT("class"), ExistingGraph->GetClass()->GetName());
			Result->SetNumberField(TEXT("element_node_count"), ExistingGraph->GetNodes().Num());
			Result->SetNumberField(TEXT("edge_count"), MonolithPCGAuthoring::GetGraphEdges(ExistingGraph).Num());
			Result->SetStringField(TEXT("existing_policy"), TEXT("return_existing"));
			return FMonolithActionResult::Success(Result);
		}
		return MonolithPCGAuthoring::InvalidParam(TEXT("asset_path"),
												  FString::Printf(TEXT("Asset already exists at '%s'"), *ObjectPath));
	}
	if (FPackageName::DoesPackageExist(PackageName))
	{
		return MonolithPCGAuthoring::InvalidParam(
			TEXT("asset_path"), FString::Printf(TEXT("A package file already exists for '%s'"), *PackageName));
	}

	UPackage* Package = CreatePackage(*PackageName);
	if (!Package)
	{
		return FMonolithActionResult::Error(FString::Printf(TEXT("CreatePackage failed for '%s'"), *PackageName));
	}

	const FString AssetName = FPackageName::GetLongPackageAssetName(PackageName);
	UPCGGraph* Graph = NewObject<UPCGGraph>(Package, UPCGGraph::StaticClass(), *AssetName,
											RF_Public | RF_Standalone | RF_Transactional);
	if (!Graph)
	{
		return FMonolithActionResult::Error(FString::Printf(TEXT("Could not create UPCGGraph '%s'"), *ObjectPath));
	}

	FAssetRegistryModule::AssetCreated(Graph);
	Graph->MarkPackageDirty();
	Graph->PostEditChange();
	TSharedPtr<FJsonObject> ValidationReport;
	if (!MonolithPCGAuthoring::ValidateGraphForCommit(Graph, ObjectPath, ValidationReport, Error))
	{
		MonolithPCGAuthoring::DiscardCreatedGraph(Graph);
		return FMonolithActionResult::Error(Error).WithErrorData(ValidationReport);
	}

	bool bSaved = false;
	FString SavedFilename;
	if (!MonolithPCGAuthoring::SaveGraph(Graph, bSave, bSaved, SavedFilename, Error))
	{
		MonolithPCGAuthoring::DiscardCreatedGraph(Graph);
		if (!SavedFilename.IsEmpty() && IFileManager::Get().FileExists(*SavedFilename) &&
			!IFileManager::Get().Delete(*SavedFilename, false, true, true))
		{
			Error += FString::Printf(TEXT("; failed to remove newly-created package residual '%s'"), *SavedFilename);
		}
		return FMonolithActionResult::Error(Error);
	}

	TSharedPtr<FJsonObject> Result = MonolithPCGAuthoring::MutationResult(TEXT("create_pcg_graph"), ObjectPath,
																		  TEXT("created"), bSaved, SavedFilename);
	Result->SetStringField(TEXT("package_name"), PackageName);
	Result->SetStringField(TEXT("class"), Graph->GetClass()->GetName());
	Result->SetStringField(TEXT("existing_policy"), ExistingPolicy.ToLower());
	Result->SetNumberField(TEXT("element_node_count"), Graph->GetNodes().Num());
	Result->SetNumberField(TEXT("edge_count"), MonolithPCGAuthoring::GetGraphEdges(Graph).Num());
	return FMonolithActionResult::Success(Result);
}

FMonolithActionResult FMonolithPCGGraphAuthoringActions::GetGraphInfo(const TSharedPtr<FJsonObject>& Params)
{
	FString AssetPath;
	FString Error;
	if (!MonolithPCGAuthoring::ReadRequiredString(Params, TEXT("asset_path"), AssetPath, Error))
	{
		return MonolithPCGAuthoring::InvalidParam(TEXT("asset_path"), Error);
	}

	bool bIncludeSettings = false;
	int32 PropertyLimit = 64;
	int32 ArrayLimit = 32;
	int32 NodeLimit = 500;
	int32 EdgeLimit = 2000;
	int32 PinLimit = 128;
	int32 ResponseItemLimit = 20000;
	TArray<FString> SettingsFields;
	if (!MonolithPCGAuthoring::ReadOptionalBool(Params, TEXT("include_settings"), false, bIncludeSettings, Error) ||
		!MonolithPCGAuthoring::ReadStringArray(Params, TEXT("settings_fields"), SettingsFields, Error) ||
		!MonolithPCGAuthoring::ReadOptionalInt(Params, TEXT("property_limit"), 64, 1, 256, PropertyLimit, Error) ||
		!MonolithPCGAuthoring::ReadOptionalInt(Params, TEXT("array_limit"), 32, 1, 256, ArrayLimit, Error) ||
		!MonolithPCGAuthoring::ReadOptionalInt(Params, TEXT("node_limit"), 500, 1, MonolithPCGAuthoring::MaxGraphNodes,
											   NodeLimit, Error) ||
		!MonolithPCGAuthoring::ReadOptionalInt(Params, TEXT("edge_limit"), 2000, 1,
											   MonolithPCGAuthoring::MaxGraphEdges, EdgeLimit, Error) ||
		!MonolithPCGAuthoring::ReadOptionalInt(Params, TEXT("pin_limit"), 128, 1,
											   MonolithPCGAuthoring::MaxPinsPerDirection, PinLimit, Error) ||
		!MonolithPCGAuthoring::ReadOptionalInt(Params, TEXT("response_item_limit"), 20000, 1,
											   MonolithPCGAuthoring::MaxGraphInfoResponseItems, ResponseItemLimit, Error))
	{
		return MonolithPCGAuthoring::InvalidParam(TEXT("params"), Error);
	}
	if (SettingsFields.Num() > 256)
	{
		return MonolithPCGAuthoring::InvalidParam(
			TEXT("settings_fields"), TEXT("settings_fields may contain at most 256 unique property names"));
	}

	UPCGGraph* Graph = nullptr;
	FString ObjectPath;
	if (!MonolithPCGAuthoring::LoadGraph(AssetPath, Graph, ObjectPath, Error))
	{
		return MonolithPCGAuthoring::InvalidParam(TEXT("asset_path"), Error);
	}

	return FMonolithActionResult::Success(MonolithPCGAuthoring::BuildGraphInfo(
		Graph, ObjectPath, bIncludeSettings, SettingsFields, PropertyLimit, ArrayLimit, NodeLimit, EdgeLimit,
		PinLimit, ResponseItemLimit));
}

FMonolithActionResult FMonolithPCGGraphAuthoringActions::AddNode(const TSharedPtr<FJsonObject>& Params)
{
	FString AssetPath;
	FString NodeType;
	FString Error;
	if (!MonolithPCGAuthoring::ReadRequiredString(Params, TEXT("asset_path"), AssetPath, Error) ||
		!MonolithPCGAuthoring::ReadRequiredString(Params, TEXT("node_type"), NodeType, Error))
	{
		return MonolithPCGAuthoring::InvalidParam(TEXT("params"), Error);
	}

	bool bSave = true;
	if (!MonolithPCGAuthoring::ReadOptionalBool(Params, TEXT("save"), true, bSave, Error))
	{
		return MonolithPCGAuthoring::InvalidParam(TEXT("save"), Error);
	}
	FString ExistingPolicy = TEXT("fail");
	if (Params.IsValid() && Params->HasField(TEXT("existing_policy")) &&
		!Params->TryGetStringField(TEXT("existing_policy"), ExistingPolicy))
	{
		return MonolithPCGAuthoring::InvalidParam(TEXT("existing_policy"), TEXT("existing_policy must be a string"));
	}
	ExistingPolicy.TrimStartAndEndInline();
	if (!ExistingPolicy.Equals(TEXT("fail"), ESearchCase::IgnoreCase) &&
		!ExistingPolicy.Equals(TEXT("return_existing"), ESearchCase::IgnoreCase))
	{
		return MonolithPCGAuthoring::InvalidParam(TEXT("existing_policy"),
												  TEXT("existing_policy must be 'fail' or 'return_existing'"));
	}

	FString NodeTitle;
	if (Params.IsValid() && Params->HasField(TEXT("node_title")) &&
		!Params->TryGetStringField(TEXT("node_title"), NodeTitle))
	{
		return MonolithPCGAuthoring::InvalidParam(TEXT("node_title"), TEXT("node_title must be a string"));
	}
	NodeTitle.TrimStartAndEndInline();

	bool bHasPosition = false;
	int32 PositionX = 0;
	int32 PositionY = 0;
	if (Params.IsValid() && Params->HasField(TEXT("position")))
	{
		const TArray<TSharedPtr<FJsonValue>>* Position = nullptr;
		if (!Params->TryGetArrayField(TEXT("position"), Position) || !Position || Position->Num() != 2)
		{
			return MonolithPCGAuthoring::InvalidParam(TEXT("position"), TEXT("position must be [x,y]"));
		}
		double X = 0.0;
		double Y = 0.0;
		if (!(*Position)[0]->TryGetNumber(X) || !(*Position)[1]->TryGetNumber(Y))
		{
			return MonolithPCGAuthoring::InvalidParam(TEXT("position"), TEXT("position must contain two numbers"));
		}
		PositionX = FMath::RoundToInt(X);
		PositionY = FMath::RoundToInt(Y);
		bHasPosition = true;
	}

	const TSharedPtr<FJsonObject>* InitialPropertiesPtr = nullptr;
	TSharedPtr<FJsonObject> InitialProperties;
	if (Params.IsValid() && Params->HasField(TEXT("properties")))
	{
		if (!Params->TryGetObjectField(TEXT("properties"), InitialPropertiesPtr) || !InitialPropertiesPtr ||
			!InitialPropertiesPtr->IsValid())
		{
			return MonolithPCGAuthoring::InvalidParam(TEXT("properties"), TEXT("properties must be an object"));
		}
		InitialProperties = *InitialPropertiesPtr;
	}

	TArray<FString> Candidates;
	UClass* SettingsClass = FMonolithPCGSettingsResolver::Resolve(NodeType, Candidates, Error);
	if (!SettingsClass)
	{
		TSharedPtr<FJsonObject> Data = MonolithPCGAuthoring::ErrorData(TEXT("node_type"), Error);
		Data->SetArrayField(TEXT("candidates"), MonolithPCGAuthoring::StringsToJson(Candidates));
		return FMonolithActionResult::Error(Error, FMonolithJsonUtils::ErrInvalidParams).WithErrorData(Data);
	}

	if (InitialProperties.IsValid())
	{
		const UPCGSettings* DefaultSettings = SettingsClass->GetDefaultObject<UPCGSettings>();
		if (!MonolithPCGAuthoring::ValidateEditableTree(DefaultSettings, InitialProperties, Error))
		{
			return MonolithPCGAuthoring::InvalidParam(TEXT("properties"), Error);
		}
		const FDryRunReport Preflight = MonolithPCGAuthoring::InspectSettingsTree(DefaultSettings, InitialProperties);
		if (Preflight.Errors > 0)
		{
			TSharedPtr<FJsonObject> Data = FMonolithDryRunGuard::ReportToJson(Preflight);
			Data->SetStringField(TEXT("field"), TEXT("properties"));
			return FMonolithActionResult::Error(TEXT("Initial PCG node properties failed strict validation"),
												FMonolithJsonUtils::ErrInvalidParams)
				.WithErrorData(Data);
		}
	}

	UPCGGraph* Graph = nullptr;
	FString ObjectPath;
	if (!MonolithPCGAuthoring::LoadGraph(AssetPath, Graph, ObjectPath, Error))
	{
		return MonolithPCGAuthoring::InvalidParam(TEXT("asset_path"), Error);
	}
	if (Graph->GetNodes().Num() >= MonolithPCGAuthoring::MaxGraphNodes)
	{
		return MonolithPCGAuthoring::InvalidParam(TEXT("asset_path"),
												  TEXT("PCG graph reached the 5000 element-node safety limit"));
	}
	const bool bWasDirty = Graph->GetPackage()->IsDirty();
	if (!NodeTitle.IsEmpty())
	{
		const TArray<UPCGNode*> ExistingTitleMatches =
			MonolithPCGAuthoring::FindNodesByAuthoredTitle(Graph, NodeTitle);
		if (ExistingTitleMatches.Num() > 1)
		{
			return MonolithPCGAuthoring::InvalidParam(
				TEXT("node_title"),
				FString::Printf(TEXT("Authored PCG node title '%s' is ambiguous across %d existing nodes; repair the graph before adding another node"),
					*NodeTitle, ExistingTitleMatches.Num()));
		}
		if (ExistingTitleMatches.Num() == 1)
		{
			UPCGNode* ExistingNode = ExistingTitleMatches[0];
			UPCGSettings* ExistingSettings = ExistingNode->GetSettings();
			if (ExistingPolicy.Equals(TEXT("return_existing"), ESearchCase::IgnoreCase))
			{
				if (!ExistingSettings || ExistingSettings->GetClass() != SettingsClass)
				{
					return MonolithPCGAuthoring::InvalidParam(
						TEXT("node_type"),
						FString::Printf(TEXT("Existing node '%s' uses %s, not requested %s"), *NodeTitle,
										ExistingSettings ? *ExistingSettings->GetClass()->GetName()
														 : TEXT("no settings"),
										*SettingsClass->GetName()));
				}
				TSharedPtr<FJsonObject> Result = MonolithPCGAuthoring::MutationResult(
					TEXT("add_pcg_node"), ObjectPath, TEXT("existing"), false, FString());
				Result->SetStringField(TEXT("node_id"), MonolithPCGAuthoring::NodeId(Graph, ExistingNode));
				Result->SetStringField(TEXT("node_path"), ExistingNode->GetPathName());
				Result->SetStringField(TEXT("node_title"), ExistingNode->GetAuthoredTitleName().ToString());
				Result->SetStringField(TEXT("settings_class"), SettingsClass->GetName());
				Result->SetStringField(TEXT("settings_class_path"), SettingsClass->GetClassPathName().ToString());
				Result->SetStringField(TEXT("existing_policy"), TEXT("return_existing"));
				MonolithPCGAuthoring::AddBoundedPinFields(Result, ExistingNode);
				return FMonolithActionResult::Success(Result);
			}
			return MonolithPCGAuthoring::InvalidParam(
				TEXT("node_title"), FString::Printf(TEXT("A PCG node already uses authored title '%s'"), *NodeTitle));
		}
	}

	UPCGSettings* Settings = nullptr;
	UPCGNode* Node = nullptr;
	TSharedPtr<FJsonObject> PropertyReport;
	{
		FMonolithPCGScopedGraphEditNotifications NotificationScope(Graph);
		Graph->Modify();
		Node = Graph->AddNodeOfType(SettingsClass, Settings);
		if (!Node || !Settings)
		{
			const bool bRolledBack = !Node || MonolithPCGAuthoring::RollbackAddedNode(Graph, Node);
			MonolithPCGAuthoring::RestorePackageDirtyState(Graph, bWasDirty);
			Error = FString::Printf(TEXT("UPCGGraph::AddNodeOfType failed for %s"), *SettingsClass->GetName());
			if (!bRolledBack)
			{
				Error += TEXT("; explicit node rollback was incomplete");
			}
			return FMonolithActionResult::Error(Error);
		}
		Node->Modify();

		if (!NodeTitle.IsEmpty())
		{
			Node->SetNodeTitle(FName(*NodeTitle));
			const FString ActualTitle = Node->HasAuthoredTitle()
				? Node->GetAuthoredTitleName().ToString()
				: FString();
			const TArray<UPCGNode*> SanitizedTitleConflicts =
				MonolithPCGAuthoring::FindNodesByAuthoredTitle(Graph, ActualTitle, Node);
			if (ActualTitle.IsEmpty() || !SanitizedTitleConflicts.IsEmpty())
			{
				const bool bRolledBack = MonolithPCGAuthoring::RollbackAddedNode(Graph, Node);
				MonolithPCGAuthoring::RestorePackageDirtyState(Graph, bWasDirty);
				Error = ActualTitle.IsEmpty()
					? TEXT("PCG settings sanitized node_title to an empty title")
					: FString::Printf(TEXT("PCG settings sanitized node_title to '%s', which conflicts with an existing node"),
						*ActualTitle);
				if (!bRolledBack)
				{
					Error += TEXT("; explicit node rollback was incomplete");
				}
				return MonolithPCGAuthoring::InvalidParam(TEXT("node_title"), Error);
			}
		}
		if (bHasPosition)
		{
			Node->SetNodePosition(PositionX, PositionY);
		}

		if (InitialProperties.IsValid())
		{
			if (!MonolithPCGAuthoring::ValidateEditableTree(Settings, InitialProperties, Error))
			{
				const bool bRolledBack = MonolithPCGAuthoring::RollbackAddedNode(Graph, Node);
				MonolithPCGAuthoring::RestorePackageDirtyState(Graph, bWasDirty);
				if (!bRolledBack)
				{
					Error += TEXT("; explicit node rollback was incomplete");
				}
				return MonolithPCGAuthoring::InvalidParam(TEXT("properties"), Error);
			}
			const FDryRunReport ApplyReport = MonolithPCGAuthoring::ApplySettingsTree(Settings, InitialProperties);
			if (ApplyReport.Errors > 0 || !ApplyReport.bWouldApply)
			{
				const bool bRolledBack = MonolithPCGAuthoring::RollbackAddedNode(Graph, Node);
				MonolithPCGAuthoring::RestorePackageDirtyState(Graph, bWasDirty);
				PropertyReport = FMonolithDryRunGuard::ReportToJson(ApplyReport);
				return FMonolithActionResult::Error(
					bRolledBack ? TEXT("PCG node property apply failed after preflight")
								: TEXT("PCG node property apply failed after preflight; explicit node rollback was incomplete"))
					.WithErrorData(PropertyReport);
			}
			Node->UpdateAfterSettingsChangeDuringCreation();
			PropertyReport = FMonolithDryRunGuard::ReportToJson(ApplyReport);
		}
		NotificationScope.MarkExternalModification();
	}

	Graph->MarkPackageDirty();
	TSharedPtr<FJsonObject> ValidationReport;
	if (!MonolithPCGAuthoring::ValidateGraphForCommit(Graph, ObjectPath, ValidationReport, Error))
	{
		const bool bRolledBack = MonolithPCGAuthoring::RollbackAddedNode(Graph, Node);
		MonolithPCGAuthoring::RestorePackageDirtyState(Graph, bWasDirty);
		if (!bRolledBack)
		{
			Error += TEXT("; explicit node rollback was incomplete");
		}
		return FMonolithActionResult::Error(Error).WithErrorData(ValidationReport);
	}
	bool bSaved = false;
	FString SavedFilename;
	if (!MonolithPCGAuthoring::SaveGraph(Graph, bSave, bSaved, SavedFilename, Error))
	{
		const bool bRolledBack = MonolithPCGAuthoring::RollbackAddedNode(Graph, Node);
		MonolithPCGAuthoring::RestorePackageDirtyState(Graph, bWasDirty);
		if (!bRolledBack)
		{
			Error += TEXT("; explicit node rollback was incomplete");
		}
		return FMonolithActionResult::Error(Error);
	}

	TSharedPtr<FJsonObject> Result =
		MonolithPCGAuthoring::MutationResult(TEXT("add_pcg_node"), ObjectPath, TEXT("added"), bSaved, SavedFilename);
	Result->SetStringField(TEXT("node_id"), MonolithPCGAuthoring::NodeId(Graph, Node));
	Result->SetStringField(TEXT("node_path"), Node->GetPathName());
	Result->SetStringField(TEXT("node_title"), Node->GetAuthoredTitleName().ToString());
	Result->SetStringField(TEXT("settings_class"), SettingsClass->GetName());
	Result->SetStringField(TEXT("settings_class_path"), SettingsClass->GetClassPathName().ToString());
	Result->SetStringField(TEXT("existing_policy"), ExistingPolicy.ToLower());
	MonolithPCGAuthoring::AddBoundedPinFields(Result, Node);
	if (PropertyReport.IsValid())
	{
		Result->SetObjectField(TEXT("property_report"), PropertyReport);
	}
	return FMonolithActionResult::Success(Result);
}

FMonolithActionResult FMonolithPCGGraphAuthoringActions::RemoveNode(const TSharedPtr<FJsonObject>& Params)
{
	FString AssetPath;
	FString NodeIdentifier;
	FString Error;
	if (!MonolithPCGAuthoring::ReadRequiredString(Params, TEXT("asset_path"), AssetPath, Error) ||
		!MonolithPCGAuthoring::ReadRequiredString(Params, TEXT("node_id"), NodeIdentifier, Error))
	{
		return MonolithPCGAuthoring::InvalidParam(TEXT("params"), Error);
	}
	bool bSave = true;
	if (!MonolithPCGAuthoring::ReadOptionalBool(Params, TEXT("save"), true, bSave, Error))
	{
		return MonolithPCGAuthoring::InvalidParam(TEXT("save"), Error);
	}

	UPCGGraph* Graph = nullptr;
	FString ObjectPath;
	if (!MonolithPCGAuthoring::LoadGraph(AssetPath, Graph, ObjectPath, Error))
	{
		return MonolithPCGAuthoring::InvalidParam(TEXT("asset_path"), Error);
	}
	UPCGNode* Node = MonolithPCGAuthoring::ResolveNode(Graph, NodeIdentifier, false, Error);
	if (!Node)
	{
		return MonolithPCGAuthoring::InvalidParam(TEXT("node_id"), Error);
	}
	const FString RemovedNodeId = MonolithPCGAuthoring::NodeId(Graph, Node);
	const FString RemovedNodeTitle = Node->GetAuthoredTitleName().ToString();
	const FName RemovedNodeName = Node->GetFName();
	const int32 BeforeEdgeCount = MonolithPCGAuthoring::GetGraphEdges(Graph).Num();
	const TArray<MonolithPCGAuthoring::FGraphEdgeDescriptor> IncidentEdges =
		MonolithPCGAuthoring::CaptureIncidentEdges(Graph, Node);
	const bool bWasDirty = Graph->GetPackage()->IsDirty();

	{
		FMonolithPCGScopedGraphEditNotifications NotificationScope(Graph);
		Graph->RemoveNode(Node);
		NotificationScope.MarkExternalModification();
	}
	Graph->MarkPackageDirty();
	if (Graph->GetNodes().Contains(Node))
	{
		MonolithPCGAuthoring::RestoreRemovedNode(Graph, Node, RemovedNodeName, IncidentEdges);
		MonolithPCGAuthoring::RestorePackageDirtyState(Graph, bWasDirty);
		return FMonolithActionResult::Error(TEXT("UPCGGraph::RemoveNode postcondition failed"));
	}
	TSharedPtr<FJsonObject> ValidationReport;
	if (!MonolithPCGAuthoring::ValidateGraphForCommit(Graph, ObjectPath, ValidationReport, Error))
	{
		const bool bRestored = MonolithPCGAuthoring::RestoreRemovedNode(
			Graph, Node, RemovedNodeName, IncidentEdges);
		MonolithPCGAuthoring::RestorePackageDirtyState(Graph, bWasDirty);
		if (!bRestored)
		{
			Error += TEXT("; explicit node rollback was incomplete");
		}
		return FMonolithActionResult::Error(Error).WithErrorData(ValidationReport);
	}

	bool bSaved = false;
	FString SavedFilename;
	if (!MonolithPCGAuthoring::SaveGraph(Graph, bSave, bSaved, SavedFilename, Error))
	{
		const bool bRestored = MonolithPCGAuthoring::RestoreRemovedNode(
			Graph, Node, RemovedNodeName, IncidentEdges);
		MonolithPCGAuthoring::RestorePackageDirtyState(Graph, bWasDirty);
		if (!bRestored)
		{
			Error += TEXT("; explicit node rollback was incomplete");
		}
		return FMonolithActionResult::Error(Error);
	}
	TSharedPtr<FJsonObject> Result = MonolithPCGAuthoring::MutationResult(TEXT("remove_pcg_node"), ObjectPath,
																		  TEXT("removed"), bSaved, SavedFilename);
	Result->SetStringField(TEXT("removed_node_id"), RemovedNodeId);
	Result->SetStringField(TEXT("removed_node_title"), RemovedNodeTitle);
	Result->SetNumberField(TEXT("removed_edge_count"),
						   BeforeEdgeCount - MonolithPCGAuthoring::GetGraphEdges(Graph).Num());
	Result->SetNumberField(TEXT("element_node_count"), Graph->GetNodes().Num());
	return FMonolithActionResult::Success(Result);
}

FMonolithActionResult FMonolithPCGGraphAuthoringActions::ConnectNodes(const TSharedPtr<FJsonObject>& Params)
{
	FString AssetPath;
	FString SourceNodeId;
	FString SourcePinLabel;
	FString TargetNodeId;
	FString TargetPinLabel;
	FString Error;
	if (!MonolithPCGAuthoring::ReadRequiredString(Params, TEXT("asset_path"), AssetPath, Error) ||
		!MonolithPCGAuthoring::ReadRequiredString(Params, TEXT("source_node"), SourceNodeId, Error) ||
		!MonolithPCGAuthoring::ReadRequiredString(Params, TEXT("source_pin"), SourcePinLabel, Error) ||
		!MonolithPCGAuthoring::ReadRequiredString(Params, TEXT("target_node"), TargetNodeId, Error) ||
		!MonolithPCGAuthoring::ReadRequiredString(Params, TEXT("target_pin"), TargetPinLabel, Error))
	{
		return MonolithPCGAuthoring::InvalidParam(TEXT("params"), Error);
	}
	bool bSave = true;
	if (!MonolithPCGAuthoring::ReadOptionalBool(Params, TEXT("save"), true, bSave, Error))
	{
		return MonolithPCGAuthoring::InvalidParam(TEXT("save"), Error);
	}

	UPCGGraph* Graph = nullptr;
	FString ObjectPath;
	if (!MonolithPCGAuthoring::LoadGraph(AssetPath, Graph, ObjectPath, Error))
	{
		return MonolithPCGAuthoring::InvalidParam(TEXT("asset_path"), Error);
	}
	UPCGNode* SourceNode = MonolithPCGAuthoring::ResolveNode(Graph, SourceNodeId, true, Error);
	if (!SourceNode)
	{
		return MonolithPCGAuthoring::InvalidParam(TEXT("source_node"), Error);
	}
	UPCGNode* TargetNode = MonolithPCGAuthoring::ResolveNode(Graph, TargetNodeId, true, Error);
	if (!TargetNode)
	{
		return MonolithPCGAuthoring::InvalidParam(TEXT("target_node"), Error);
	}
	if (!MonolithPCGAuthoring::IsGraphNode(Graph, SourceNode) || !MonolithPCGAuthoring::IsGraphNode(Graph, TargetNode))
	{
		return MonolithPCGAuthoring::InvalidParam(TEXT("nodes"), TEXT("Both nodes must belong to the target graph"));
	}

	UPCGPin* SourcePin = SourceNode->GetOutputPin(FName(*SourcePinLabel));
	UPCGPin* TargetPin = TargetNode->GetInputPin(FName(*TargetPinLabel));
	if (!SourcePin)
	{
		return MonolithPCGAuthoring::InvalidParam(
			TEXT("source_pin"),
			FString::Printf(TEXT("Source node '%s' has no output pin '%s'"), *SourceNodeId, *SourcePinLabel));
	}
	if (!TargetPin)
	{
		return MonolithPCGAuthoring::InvalidParam(
			TEXT("target_pin"),
			FString::Printf(TEXT("Target node '%s' has no input pin '%s'"), *TargetNodeId, *TargetPinLabel));
	}
	if (!SourcePin->IsOutputPin() || TargetPin->IsOutputPin())
	{
		return MonolithPCGAuthoring::InvalidParam(TEXT("pins"),
												  TEXT("Connections must run from an output pin to an input pin"));
	}
	const EPCGDataTypeCompatibilityResult Compatibility = SourcePin->GetCompatibilityWithOtherPin(TargetPin);
	if (Compatibility != EPCGDataTypeCompatibilityResult::Compatible)
	{
		const FString Detail = Compatibility == EPCGDataTypeCompatibilityResult::RequireFilter
			? TEXT("Connection requires an explicit PCG filter node")
			: Compatibility == EPCGDataTypeCompatibilityResult::RequireConversion
				? TEXT("Connection requires an explicit PCG conversion node")
				: TEXT("Source and target PCG pin data types are incompatible");
		return MonolithPCGAuthoring::InvalidParam(TEXT("pins"), Detail);
	}

	const bool bAlreadyConnected = MonolithPCGAuthoring::AreConnected(SourcePin, TargetPin);
	if (!bAlreadyConnected && MonolithPCGAuthoring::WouldCreateCycle(SourceNode, TargetNode))
	{
		return MonolithPCGAuthoring::InvalidParam(
			TEXT("nodes"), SourceNode == TargetNode ? TEXT("A PCG node cannot connect to itself")
											 : TEXT("The requested PCG edge would create a cycle"));
	}
	if (!bAlreadyConnected && !TargetPin->CanConnect(SourcePin))
	{
		return MonolithPCGAuthoring::InvalidParam(
			TEXT("target_pin"),
			FString::Printf(TEXT("Target input pin '%s' already has a connection and does not allow implicit replacement; disconnect it explicitly first"),
				*TargetPinLabel));
	}
	if (!bAlreadyConnected && MonolithPCGAuthoring::GetGraphEdges(Graph).Num() >= MonolithPCGAuthoring::MaxGraphEdges)
	{
		return MonolithPCGAuthoring::InvalidParam(TEXT("asset_path"),
			TEXT("PCG graph reached the 20000-edge safety limit"));
	}

	const bool bWasDirty = Graph->GetPackage()->IsDirty();
	if (!bAlreadyConnected)
	{
		FMonolithPCGScopedGraphEditNotifications NotificationScope(Graph);
		Graph->AddLabeledEdge(SourceNode, FName(*SourcePinLabel), TargetNode, FName(*TargetPinLabel));
		if (!MonolithPCGAuthoring::AreConnected(SourcePin, TargetPin))
		{
			MonolithPCGAuthoring::RollbackConnectedEdge(
				Graph, SourceNode, FName(*SourcePinLabel), TargetNode, FName(*TargetPinLabel));
			MonolithPCGAuthoring::RestorePackageDirtyState(Graph, bWasDirty);
			return FMonolithActionResult::Error(TEXT("UPCGGraph::AddEdge did not create the requested edge"));
		}
		NotificationScope.MarkExternalModification();
		Graph->MarkPackageDirty();
	}
	TSharedPtr<FJsonObject> ValidationReport;
	if (!MonolithPCGAuthoring::ValidateGraphForCommit(Graph, ObjectPath, ValidationReport, Error))
	{
		if (!bAlreadyConnected)
		{
			MonolithPCGAuthoring::RollbackConnectedEdge(
				Graph, SourceNode, FName(*SourcePinLabel), TargetNode, FName(*TargetPinLabel));
			MonolithPCGAuthoring::RestorePackageDirtyState(Graph, bWasDirty);
		}
		return FMonolithActionResult::Error(Error).WithErrorData(ValidationReport);
	}

	bool bSaved = false;
	FString SavedFilename;
	if (!MonolithPCGAuthoring::SaveGraph(Graph, bSave && !bAlreadyConnected, bSaved, SavedFilename, Error))
	{
		if (!bAlreadyConnected)
		{
			MonolithPCGAuthoring::RollbackConnectedEdge(
				Graph, SourceNode, FName(*SourcePinLabel), TargetNode, FName(*TargetPinLabel));
			MonolithPCGAuthoring::RestorePackageDirtyState(Graph, bWasDirty);
		}
		return FMonolithActionResult::Error(Error);
	}
	TSharedPtr<FJsonObject> Result = MonolithPCGAuthoring::MutationResult(
		TEXT("connect_pcg_nodes"), ObjectPath, bAlreadyConnected ? TEXT("already_connected") : TEXT("connected"),
		bSaved, SavedFilename);
	Result->SetBoolField(TEXT("created"), !bAlreadyConnected);
	Result->SetStringField(TEXT("source_node"), MonolithPCGAuthoring::NodeId(Graph, SourceNode));
	Result->SetStringField(TEXT("source_pin"), SourcePinLabel);
	Result->SetStringField(TEXT("target_node"), MonolithPCGAuthoring::NodeId(Graph, TargetNode));
	Result->SetStringField(TEXT("target_pin"), TargetPinLabel);
	Result->SetNumberField(TEXT("edge_count"), MonolithPCGAuthoring::GetGraphEdges(Graph).Num());
	return FMonolithActionResult::Success(Result);
}

FMonolithActionResult FMonolithPCGGraphAuthoringActions::DisconnectNodes(const TSharedPtr<FJsonObject>& Params)
{
	FString AssetPath;
	FString SourceNodeId;
	FString SourcePinLabel;
	FString TargetNodeId;
	FString TargetPinLabel;
	FString Error;
	if (!MonolithPCGAuthoring::ReadRequiredString(Params, TEXT("asset_path"), AssetPath, Error) ||
		!MonolithPCGAuthoring::ReadRequiredString(Params, TEXT("source_node"), SourceNodeId, Error) ||
		!MonolithPCGAuthoring::ReadRequiredString(Params, TEXT("source_pin"), SourcePinLabel, Error) ||
		!MonolithPCGAuthoring::ReadRequiredString(Params, TEXT("target_node"), TargetNodeId, Error) ||
		!MonolithPCGAuthoring::ReadRequiredString(Params, TEXT("target_pin"), TargetPinLabel, Error))
	{
		return MonolithPCGAuthoring::InvalidParam(TEXT("params"), Error);
	}
	bool bSave = true;
	if (!MonolithPCGAuthoring::ReadOptionalBool(Params, TEXT("save"), true, bSave, Error))
	{
		return MonolithPCGAuthoring::InvalidParam(TEXT("save"), Error);
	}

	UPCGGraph* Graph = nullptr;
	FString ObjectPath;
	if (!MonolithPCGAuthoring::LoadGraph(AssetPath, Graph, ObjectPath, Error))
	{
		return MonolithPCGAuthoring::InvalidParam(TEXT("asset_path"), Error);
	}
	UPCGNode* SourceNode = MonolithPCGAuthoring::ResolveNode(Graph, SourceNodeId, true, Error);
	if (!SourceNode)
	{
		return MonolithPCGAuthoring::InvalidParam(TEXT("source_node"), Error);
	}
	UPCGNode* TargetNode = MonolithPCGAuthoring::ResolveNode(Graph, TargetNodeId, true, Error);
	if (!TargetNode)
	{
		return MonolithPCGAuthoring::InvalidParam(TEXT("target_node"), Error);
	}
	UPCGPin* SourcePin = SourceNode->GetOutputPin(FName(*SourcePinLabel));
	UPCGPin* TargetPin = TargetNode->GetInputPin(FName(*TargetPinLabel));
	if (!SourcePin || !TargetPin)
	{
		return MonolithPCGAuthoring::InvalidParam(
			TEXT("pins"), TEXT("The requested source output or target input pin does not exist"));
	}

	const bool bWasConnected = MonolithPCGAuthoring::AreConnected(SourcePin, TargetPin);
	const bool bWasDirty = Graph->GetPackage()->IsDirty();
	if (bWasConnected)
	{
		FMonolithPCGScopedGraphEditNotifications NotificationScope(Graph);
		const bool bRemoved = Graph->RemoveEdge(SourceNode, FName(*SourcePinLabel), TargetNode, FName(*TargetPinLabel));
		if (!bRemoved || MonolithPCGAuthoring::AreConnected(SourcePin, TargetPin))
		{
			MonolithPCGAuthoring::RollbackDisconnectedEdge(
				Graph, SourceNode, FName(*SourcePinLabel), TargetNode, FName(*TargetPinLabel));
			MonolithPCGAuthoring::RestorePackageDirtyState(Graph, bWasDirty);
			return FMonolithActionResult::Error(TEXT("UPCGGraph::RemoveEdge postcondition failed"));
		}
		NotificationScope.MarkExternalModification();
		Graph->MarkPackageDirty();
	}
	TSharedPtr<FJsonObject> ValidationReport;
	if (!MonolithPCGAuthoring::ValidateGraphForCommit(Graph, ObjectPath, ValidationReport, Error))
	{
		if (bWasConnected)
		{
			const bool bRestored = MonolithPCGAuthoring::RollbackDisconnectedEdge(
				Graph, SourceNode, FName(*SourcePinLabel), TargetNode, FName(*TargetPinLabel));
			MonolithPCGAuthoring::RestorePackageDirtyState(Graph, bWasDirty);
			if (!bRestored)
			{
				Error += TEXT("; explicit edge rollback was incomplete");
			}
		}
		return FMonolithActionResult::Error(Error).WithErrorData(ValidationReport);
	}

	bool bSaved = false;
	FString SavedFilename;
	if (!MonolithPCGAuthoring::SaveGraph(Graph, bSave && bWasConnected, bSaved, SavedFilename, Error))
	{
		if (bWasConnected)
		{
			const bool bRestored = MonolithPCGAuthoring::RollbackDisconnectedEdge(
				Graph, SourceNode, FName(*SourcePinLabel), TargetNode, FName(*TargetPinLabel));
			MonolithPCGAuthoring::RestorePackageDirtyState(Graph, bWasDirty);
			if (!bRestored)
			{
				Error += TEXT("; explicit edge rollback was incomplete");
			}
		}
		return FMonolithActionResult::Error(Error);
	}
	TSharedPtr<FJsonObject> Result = MonolithPCGAuthoring::MutationResult(
		TEXT("disconnect_pcg_nodes"), ObjectPath, bWasConnected ? TEXT("disconnected") : TEXT("not_connected"), bSaved,
		SavedFilename);
	Result->SetBoolField(TEXT("removed"), bWasConnected);
	Result->SetStringField(TEXT("source_node"), MonolithPCGAuthoring::NodeId(Graph, SourceNode));
	Result->SetStringField(TEXT("source_pin"), SourcePinLabel);
	Result->SetStringField(TEXT("target_node"), MonolithPCGAuthoring::NodeId(Graph, TargetNode));
	Result->SetStringField(TEXT("target_pin"), TargetPinLabel);
	Result->SetNumberField(TEXT("edge_count"), MonolithPCGAuthoring::GetGraphEdges(Graph).Num());
	return FMonolithActionResult::Success(Result);
}

FMonolithActionResult FMonolithPCGGraphAuthoringActions::SetNodeParams(const TSharedPtr<FJsonObject>& Params)
{
	FString AssetPath;
	FString NodeIdentifier;
	FString Error;
	if (!MonolithPCGAuthoring::ReadRequiredString(Params, TEXT("asset_path"), AssetPath, Error) ||
		!MonolithPCGAuthoring::ReadRequiredString(Params, TEXT("node_id"), NodeIdentifier, Error))
	{
		return MonolithPCGAuthoring::InvalidParam(TEXT("params"), Error);
	}
	const TSharedPtr<FJsonObject>* PropertiesPtr = nullptr;
	if (!Params.IsValid() || !Params->TryGetObjectField(TEXT("properties"), PropertiesPtr) || !PropertiesPtr ||
		!PropertiesPtr->IsValid())
	{
		return MonolithPCGAuthoring::InvalidParam(TEXT("properties"), TEXT("properties must be an object"));
	}
	const TSharedPtr<FJsonObject> Properties = *PropertiesPtr;

	bool bDryRun = false;
	bool bSave = true;
	if (!MonolithPCGAuthoring::ReadOptionalBool(Params, TEXT("dry_run"), false, bDryRun, Error) ||
		!MonolithPCGAuthoring::ReadOptionalBool(Params, TEXT("save"), true, bSave, Error))
	{
		return MonolithPCGAuthoring::InvalidParam(TEXT("params"), Error);
	}

	UPCGGraph* Graph = nullptr;
	FString ObjectPath;
	if (!MonolithPCGAuthoring::LoadGraph(AssetPath, Graph, ObjectPath, Error))
	{
		return MonolithPCGAuthoring::InvalidParam(TEXT("asset_path"), Error);
	}
	UPCGNode* Node = MonolithPCGAuthoring::ResolveNode(Graph, NodeIdentifier, false, Error);
	if (!Node)
	{
		return MonolithPCGAuthoring::InvalidParam(TEXT("node_id"), Error);
	}
	UPCGSettings* Settings = Node->GetSettings();
	if (!Settings)
	{
		return FMonolithActionResult::Error(
			FString::Printf(TEXT("PCG node '%s' has no settings object"), *NodeIdentifier));
	}
	if (!MonolithPCGAuthoring::ValidateEditableTree(Settings, Properties, Error))
	{
		return MonolithPCGAuthoring::InvalidParam(TEXT("properties"), Error);
	}

	FDryRunReport Report = MonolithPCGAuthoring::InspectSettingsTree(Settings, Properties);
	if (Report.Errors > 0)
	{
		TSharedPtr<FJsonObject> Data = FMonolithDryRunGuard::ReportToJson(Report);
		Data->SetStringField(TEXT("field"), TEXT("properties"));
		return FMonolithActionResult::Error(TEXT("PCG node properties failed strict validation"),
											FMonolithJsonUtils::ErrInvalidParams)
			.WithErrorData(Data);
	}

	if (bDryRun)
	{
		TSharedPtr<FJsonObject> Result = FMonolithDryRunGuard::ReportToJson(Report);
		Result->SetStringField(TEXT("namespace"), TEXT("pcg"));
		Result->SetStringField(TEXT("action"), TEXT("set_pcg_node_params"));
		Result->SetStringField(TEXT("status"), TEXT("dry_run"));
		Result->SetStringField(TEXT("asset_path"), ObjectPath);
		Result->SetStringField(TEXT("node_id"), MonolithPCGAuthoring::NodeId(Graph, Node));
		Result->SetStringField(TEXT("settings_class"), Settings->GetClass()->GetName());
		Result->SetBoolField(TEXT("dry_run"), true);
		Result->SetBoolField(TEXT("valid_to_apply"), Report.Errors == 0 && Report.SilentDrops.IsEmpty());
		return FMonolithActionResult::Success(Result);
	}

	MonolithPCGAuthoring::FSettingsSnapshots Snapshots;
	if (!MonolithPCGAuthoring::CaptureSettingsSnapshots(Settings, Properties, Snapshots, Error))
	{
		return FMonolithActionResult::Error(Error);
	}
	const TArray<MonolithPCGAuthoring::FGraphEdgeDescriptor> OriginalIncidentEdges =
		MonolithPCGAuthoring::CaptureIncidentEdges(Graph, Node);
	const bool bWasDirty = Graph->GetPackage()->IsDirty();
	{
		FMonolithPCGScopedGraphEditNotifications NotificationScope(Graph);
		Report = MonolithPCGAuthoring::ApplySettingsTree(Settings, Properties);
		NotificationScope.MarkExternalModification();
	}
	if (Report.Errors > 0 || !Report.bWouldApply)
	{
		const bool bRestored = MonolithPCGAuthoring::RestoreSettingsMutation(
			Graph, Node, Settings, Snapshots, OriginalIncidentEdges, bWasDirty);
		return FMonolithActionResult::Error(
			bRestored ? TEXT("PCG node property apply failed")
						  : TEXT("PCG node property apply failed; explicit settings rollback was incomplete"))
			.WithErrorData(FMonolithDryRunGuard::ReportToJson(Report));
	}
	Graph->MarkPackageDirty();
	TSharedPtr<FJsonObject> ValidationReport;
	if (!MonolithPCGAuthoring::ValidateGraphForCommit(Graph, ObjectPath, ValidationReport, Error))
	{
		const bool bRestored = MonolithPCGAuthoring::RestoreSettingsMutation(
			Graph, Node, Settings, Snapshots, OriginalIncidentEdges, bWasDirty);
		if (!bRestored)
		{
			Error += TEXT("; explicit settings rollback was incomplete");
		}
		return FMonolithActionResult::Error(Error).WithErrorData(ValidationReport);
	}

	bool bSaved = false;
	FString SavedFilename;
	if (!MonolithPCGAuthoring::SaveGraph(Graph, bSave, bSaved, SavedFilename, Error))
	{
		const bool bRestored = MonolithPCGAuthoring::RestoreSettingsMutation(
			Graph, Node, Settings, Snapshots, OriginalIncidentEdges, bWasDirty);
		if (!bRestored)
		{
			Error += TEXT("; explicit settings rollback was incomplete");
		}
		return FMonolithActionResult::Error(Error);
	}
	TSharedPtr<FJsonObject> Result = FMonolithDryRunGuard::ReportToJson(Report);
	Result->SetStringField(TEXT("namespace"), TEXT("pcg"));
	Result->SetStringField(TEXT("action"), TEXT("set_pcg_node_params"));
	Result->SetStringField(TEXT("status"), TEXT("updated"));
	Result->SetStringField(TEXT("asset_path"), ObjectPath);
	Result->SetStringField(TEXT("node_id"), MonolithPCGAuthoring::NodeId(Graph, Node));
	Result->SetStringField(TEXT("settings_class"), Settings->GetClass()->GetName());
	Result->SetBoolField(TEXT("dry_run"), false);
	Result->SetBoolField(TEXT("saved"), bSaved);
	if (!SavedFilename.IsEmpty())
	{
		Result->SetStringField(TEXT("saved_filename"), SavedFilename);
	}
	return FMonolithActionResult::Success(Result);
}

FMonolithActionResult FMonolithPCGGraphAuthoringActions::ValidateGraph(const TSharedPtr<FJsonObject>& Params)
{
	FString AssetPath;
	FString Error;
	if (!MonolithPCGAuthoring::ReadRequiredString(Params, TEXT("asset_path"), AssetPath, Error))
	{
		return MonolithPCGAuthoring::InvalidParam(TEXT("asset_path"), Error);
	}
	bool bRequireOutputConnection = false;
	bool bRequireNoIsolatedNodes = false;
	int32 IssueLimit = 200;
	if (!MonolithPCGAuthoring::ReadOptionalBool(Params, TEXT("require_output_connection"), false,
												bRequireOutputConnection, Error) ||
		!MonolithPCGAuthoring::ReadOptionalBool(Params, TEXT("require_no_isolated_nodes"), false,
												bRequireNoIsolatedNodes, Error) ||
		!MonolithPCGAuthoring::ReadOptionalInt(Params, TEXT("issue_limit"), 200, 1,
											   MonolithPCGAuthoring::MaxValidationIssues, IssueLimit, Error))
	{
		return MonolithPCGAuthoring::InvalidParam(TEXT("params"), Error);
	}

	UPCGGraph* Graph = nullptr;
	FString ObjectPath;
	if (!MonolithPCGAuthoring::LoadGraph(AssetPath, Graph, ObjectPath, Error))
	{
		return MonolithPCGAuthoring::InvalidParam(TEXT("asset_path"), Error);
	}

	return FMonolithActionResult::Success(MonolithPCGAuthoring::BuildGraphValidationReport(
		Graph, ObjectPath, bRequireOutputConnection, bRequireNoIsolatedNodes, IssueLimit));
}
