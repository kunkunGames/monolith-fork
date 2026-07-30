#include "MonolithPCGGraphAuthoringActions.h"

#include "MonolithPCGGraphEditScope.h"
#include "MonolithPCGSettingsResolver.h"

#include "MonolithAssetUtils.h"
#include "MonolithJsonUtils.h"
#include "MonolithParamSchema.h"
#include "MonolithPCGPropertyBagUtils.h"
#include "MonolithPCGResultUtils.h"
#include "MonolithSourceControlUtils.h"
#include "Reflection/MonolithDryRunGuard.h"
#include "Reflection/MonolithReflectionReader.h"
#include "Reflection/MonolithReflectionWalker.h"

#include "PCGCommon.h"
#include "PCGEdge.h"
#include "PCGGraph.h"
#include "PCGNode.h"
#include "PCGPin.h"
#include "PCGSettings.h"
#include "PCGSubgraph.h"
#include "Data/Registry/PCGDataTypeCommon.h"

#include "AssetRegistry/IAssetRegistry.h"
#include "AssetRegistry/AssetRegistryModule.h"
#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "Editor.h"
#include "HAL/FileManager.h"
#include "Misc/EngineVersionComparison.h"
#include "Misc/PackageName.h"
#include "Misc/Paths.h"
#include "Misc/ScopeExit.h"
#include "Misc/StringOutputDevice.h"
#include "Modules/ModuleManager.h"
#include "Serialization/MemoryWriter.h"
#include "Serialization/ObjectAndNameAsStringProxyArchive.h"
#include "Serialization/StructuredArchiveAdapters.h"
#include "Subsystems/EditorAssetSubsystem.h"
#include "UObject/Package.h"
#include "UObject/PropertyPortFlags.h"
#include "UObject/SavePackage.h"
#include "UObject/SoftObjectPath.h"
#include "UObject/UObjectGlobals.h"
#include "UObject/UObjectHash.h"
#include "UObject/UObjectHash.h"
#include "UObject/UnrealType.h"

#if WITH_DEV_AUTOMATION_TESTS
namespace UE::MonolithPCG::Private
{
namespace
{
FString GGraphContentsFaultTarget;
EPCGGraphContentsReplacementTestFault GGraphContentsFault =
	EPCGGraphContentsReplacementTestFault::None;
}

void ConfigureGraphContentsReplacementTestFault(
	const FString& ExactTargetObjectPath,
	EPCGGraphContentsReplacementTestFault Fault)
{
	GGraphContentsFaultTarget = ExactTargetObjectPath;
	GGraphContentsFault = Fault;
}

void ResetGraphContentsReplacementTestFault()
{
	GGraphContentsFaultTarget.Reset();
	GGraphContentsFault = EPCGGraphContentsReplacementTestFault::None;
}

bool ConsumeGraphContentsReplacementTestFault(
	const FString& ExactTargetObjectPath,
	EPCGGraphContentsReplacementTestFault Fault)
{
	if (GGraphContentsFault != Fault ||
		!GGraphContentsFaultTarget.Equals(ExactTargetObjectPath, ESearchCase::CaseSensitive))
	{
		return false;
	}
	GGraphContentsFault = EPCGGraphContentsReplacementTestFault::None;
	return true;
}

bool TryBuildGraphContentsReplacementTestSourceControl(
	const FString& ExactTargetObjectPath,
	TSharedPtr<FJsonObject>& OutPrepare)
{
	if (GGraphContentsFaultTarget.IsEmpty() ||
		!GGraphContentsFaultTarget.Equals(ExactTargetObjectPath, ESearchCase::CaseSensitive))
	{
		return false;
	}
	TSharedPtr<FJsonObject> BeforeAction = MakeShared<FJsonObject>();
	BeforeAction->SetBoolField(TEXT("ok"), true);
	BeforeAction->SetBoolField(TEXT("available"), true);
	BeforeAction->SetStringField(TEXT("status"), TEXT("prepared_test_fixture"));
	BeforeAction->SetStringField(TEXT("provider"), TEXT("MonolithPCGReplacementFixture"));
	OutPrepare = MakeShared<FJsonObject>();
	OutPrepare->SetStringField(TEXT("mode"), TEXT("handler_owned_pre_mutation"));
	OutPrepare->SetStringField(TEXT("status"), TEXT("prepared"));
	OutPrepare->SetObjectField(TEXT("before_action"), BeforeAction);
	return true;
}
}
#endif

namespace MonolithPCGAuthoring
{
static constexpr int32 MaxGraphNodes = 5000;
static constexpr int32 MaxGraphEdges = 20000;
static constexpr int32 MaxValidationIssues = 1000;
static constexpr int32 MaxNodeTypes = 1000;
static constexpr int32 MaxPinsPerDirection = 1024;
static constexpr int32 MaxGraphInfoResponseItems = 100000;
static constexpr int32 MaxGraphUserParameterOperations = 256;
static constexpr int32 MaxGraphUserParameterStringChars = 4096;
static constexpr double MaxExactJsonInteger = 9007199254740991.0; // 2^53 - 1
// Whole-graph replacement already caps authored topology. These independent
// reflection bounds prevent a malformed graph with a small node count but an
// unbounded inner-object tree or property payload from monopolizing the editor.
static constexpr int32 MaxPersistentGraphObjects = 250000;
static constexpr int32 MaxPersistentGraphObjectDepth = 256;
static constexpr int64 MaxPersistentGraphSerializedBytes = 128ll * 1024ll * 1024ll;

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
	const TSharedPtr<FJsonValue> JsonValue = Params.IsValid() ? Params->TryGetField(Field) : nullptr;
	if (!JsonValue.IsValid() || JsonValue->Type != EJson::String || !JsonValue->TryGetString(OutValue))
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
	const TSharedPtr<FJsonValue> JsonValue = Params->TryGetField(Field);
	if (!JsonValue.IsValid() || JsonValue->Type != EJson::Boolean || !JsonValue->TryGetBool(OutValue))
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
	if (!FMath::IsFinite(Number) ||
		Number < static_cast<double>(MinValue) ||
		Number > static_cast<double>(MaxValue) ||
		FMath::TruncToDouble(Number) != Number)
	{
		OutError = FString::Printf(TEXT("%s must be an integer in range %d..%d"), Field, MinValue, MaxValue);
		return false;
	}
	OutValue = static_cast<int32>(Number);
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
		if (!Value.IsValid() || Value->Type != EJson::String || !Value->TryGetString(StringValue))
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

	if (!FPackageName::IsValidLongPackageName(OutPackageName) ||
		!FMonolithAssetUtils::IsProjectOwnedPackage(OutPackageName))
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
	const FString LoadedObjectPath = OutGraph ? OutGraph->GetPathName() : FString();
	if (!LoadedObjectPath.Equals(OutObjectPath, ESearchCase::CaseSensitive))
	{
		OutError = FString::Printf(
			TEXT("asset_path must be the exact canonical object path; aliases, redirectors, and "
				 "case-only variants are rejected (requested '%s', resolved '%s')"),
			*OutObjectPath,
			*LoadedObjectPath);
		OutGraph = nullptr;
		return false;
	}
	return true;
}

FString GraphUserParameterTypeToString(EPropertyBagPropertyType Type)
{
	switch (Type)
	{
	case EPropertyBagPropertyType::Bool: return TEXT("bool");
	case EPropertyBagPropertyType::Byte: return TEXT("byte");
	case EPropertyBagPropertyType::Int32: return TEXT("int32");
	case EPropertyBagPropertyType::Int64: return TEXT("int64");
	case EPropertyBagPropertyType::Float: return TEXT("float");
	case EPropertyBagPropertyType::Double: return TEXT("double");
	case EPropertyBagPropertyType::Name: return TEXT("name");
	case EPropertyBagPropertyType::String: return TEXT("string");
	default: return TEXT("unsupported");
	}
}

bool ParseGraphUserParameterType(const FString& Input, EPropertyBagPropertyType& OutType)
{
	if (Input.Equals(TEXT("bool"), ESearchCase::IgnoreCase)) { OutType = EPropertyBagPropertyType::Bool; return true; }
	if (Input.Equals(TEXT("byte"), ESearchCase::IgnoreCase)) { OutType = EPropertyBagPropertyType::Byte; return true; }
	if (Input.Equals(TEXT("int32"), ESearchCase::IgnoreCase)) { OutType = EPropertyBagPropertyType::Int32; return true; }
	if (Input.Equals(TEXT("int64"), ESearchCase::IgnoreCase)) { OutType = EPropertyBagPropertyType::Int64; return true; }
	if (Input.Equals(TEXT("float"), ESearchCase::IgnoreCase)) { OutType = EPropertyBagPropertyType::Float; return true; }
	if (Input.Equals(TEXT("double"), ESearchCase::IgnoreCase)) { OutType = EPropertyBagPropertyType::Double; return true; }
	if (Input.Equals(TEXT("name"), ESearchCase::IgnoreCase)) { OutType = EPropertyBagPropertyType::Name; return true; }
	if (Input.Equals(TEXT("string"), ESearchCase::IgnoreCase)) { OutType = EPropertyBagPropertyType::String; return true; }
	return false;
}

bool GraphUserParameterJsonToSerialized(EPropertyBagPropertyType Type,
	const TSharedPtr<FJsonValue>& JsonValue, FString& OutSerialized, FString& OutError)
{
	if (!JsonValue.IsValid() || JsonValue->IsNull())
	{
		OutError = TEXT("default_value must not be null");
		return false;
	}
	switch (Type)
	{
	case EPropertyBagPropertyType::Bool:
	{
		bool Value = false;
		if (JsonValue->Type != EJson::Boolean || !JsonValue->TryGetBool(Value))
		{
			OutError = TEXT("expected a JSON boolean");
			return false;
		}
		OutSerialized = Value ? TEXT("True") : TEXT("False");
		return true;
	}
	case EPropertyBagPropertyType::Int64:
	{
		if (JsonValue->Type == EJson::String)
		{
			FString Decimal;
			JsonValue->TryGetString(Decimal);
			int64 Parsed = 0;
			LexFromString(Parsed, FStringView(Decimal));
			if (LexToString(Parsed) != Decimal)
			{
				OutError = TEXT("expected a canonical decimal int64 string in signed 64-bit range");
				return false;
			}
			OutSerialized = MoveTemp(Decimal);
			return true;
		}
		double Value = 0.0;
		if (JsonValue->Type != EJson::Number || !JsonValue->TryGetNumber(Value) || !FMath::IsFinite(Value) ||
			FMath::TruncToDouble(Value) != Value || FMath::Abs(Value) > MaxExactJsonInteger)
		{
			OutError = TEXT("expected an exact integral JSON number or canonical decimal int64 string");
			return false;
		}
		OutSerialized = FString::Printf(TEXT("%.0f"), Value);
		return true;
	}
	case EPropertyBagPropertyType::Byte:
	case EPropertyBagPropertyType::Int32:
	{
		double Value = 0.0;
		if (JsonValue->Type != EJson::Number || !JsonValue->TryGetNumber(Value) || !FMath::IsFinite(Value) ||
			FMath::TruncToDouble(Value) != Value)
		{
			OutError = TEXT("expected an integral JSON number");
			return false;
		}
		if (Type == EPropertyBagPropertyType::Byte && (Value < 0.0 || Value > 255.0))
		{
			OutError = TEXT("byte value must be in range 0..255");
			return false;
		}
		if (Type == EPropertyBagPropertyType::Int32 &&
			(Value < static_cast<double>(MIN_int32) || Value > static_cast<double>(MAX_int32)))
		{
			OutError = TEXT("int32 value is out of range");
			return false;
		}
		OutSerialized = FString::Printf(TEXT("%.0f"), Value);
		return true;
	}
	case EPropertyBagPropertyType::Float:
	case EPropertyBagPropertyType::Double:
	{
		double Value = 0.0;
		if (JsonValue->Type != EJson::Number || !JsonValue->TryGetNumber(Value) || !FMath::IsFinite(Value) ||
			(Type == EPropertyBagPropertyType::Float &&
			 FMath::Abs(Value) > static_cast<double>(TNumericLimits<float>::Max())))
		{
			OutError = TEXT("expected a finite in-range JSON number");
			return false;
		}
		OutSerialized = FString::Printf(TEXT("%.17g"), Value);
		return true;
	}
	case EPropertyBagPropertyType::Name:
	case EPropertyBagPropertyType::String:
	{
		if (JsonValue->Type != EJson::String || !JsonValue->TryGetString(OutSerialized))
		{
			OutError = TEXT("expected a JSON string");
			return false;
		}
		if (OutSerialized.Len() > MaxGraphUserParameterStringChars)
		{
			OutError = FString::Printf(TEXT("string exceeds the %d-character limit"),
				MaxGraphUserParameterStringChars);
			return false;
		}
		return true;
	}
	default:
		OutError = TEXT("unsupported graph user-parameter type");
		return false;
	}
}

bool LoadGraphInterface(const FString& InputPath, UPCGGraphInterface*& OutInterface, FString& OutObjectPath,
						FString& OutError)
{
	OutInterface = nullptr;
	FString PackageName;
	if (!NormalizeGraphPath(InputPath, PackageName, OutObjectPath, OutError))
	{
		return false;
	}

	FString ResolvedPath;
	if (!FMonolithAssetUtils::TryLoadAssetByPath<UPCGGraphInterface>(
			OutObjectPath, OutInterface, ResolvedPath, OutError))
	{
		OutError = FString::Printf(TEXT("Could not load PCG graph interface '%s': %s"), *OutObjectPath, *OutError);
		return false;
	}
	const FString LoadedObjectPath = OutInterface ? OutInterface->GetPathName() : FString();
	if (!LoadedObjectPath.Equals(OutObjectPath, ESearchCase::CaseSensitive))
	{
		OutError = FString::Printf(
			TEXT("subgraph_asset_path must be the exact canonical object path; aliases, redirectors, and "
				 "case-only variants are rejected (requested '%s', resolved '%s')"),
			*OutObjectPath,
			*LoadedObjectPath);
		OutInterface = nullptr;
		return false;
	}
	if (!OutInterface || !OutInterface->GetGraph())
	{
		OutError = FString::Printf(TEXT("PCG graph interface '%s' has no concrete graph"), *OutObjectPath);
		OutInterface = nullptr;
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
	TSharedPtr<FJsonValue> LeafValue;
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
			Unit.LeafValue = Pair.Value;
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

bool ValidateClassPropertyUnit(const UPCGSettings* Settings, const FSettingsWriteUnit& Unit, FString& OutError)
{
	if (Unit.PropertyChain.IsEmpty())
	{
		return true;
	}

	FClassProperty* ClassProperty = CastField<FClassProperty>(Unit.PropertyChain.Last());
	if (!ClassProperty)
	{
		return true;
	}
	if (!Unit.LeafValue.IsValid() || Unit.LeafValue->Type != EJson::String)
	{
		OutError = FString::Printf(
			TEXT("Class settings property '%s' requires an exact class object-path string"),
			*Unit.Path);
		return false;
	}

	const FString ClassPath = Unit.LeafValue->AsString();
	if (ClassPath.IsEmpty())
	{
		return true;
	}

	void* Scratch = FMemory::Malloc(ClassProperty->GetSize(), FMath::Max(1, ClassProperty->GetMinAlignment()));
	ClassProperty->InitializeValue(Scratch);
	FStringOutputDevice ErrorText;
	const TCHAR* ImportResult = ClassProperty->ImportText_Direct(
		*ClassPath,
		Scratch,
		const_cast<UPCGSettings*>(Settings),
		PPF_None,
		&ErrorText);
	ClassProperty->DestroyValue(Scratch);
	FMemory::Free(Scratch);

	if (!ImportResult)
	{
		OutError = FString::Printf(
			TEXT("Class settings property '%s' rejected '%s'%s%s"),
			*Unit.Path,
			*ClassPath,
			ErrorText.IsEmpty() ? TEXT("") : TEXT(": "),
			*ErrorText);
		return false;
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
		if (!ValidateClassPropertyUnit(Settings, Unit, OutError))
		{
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
	for (const auto& Pair : Tree->Values)
	{
		FProperty* Property = FMonolithReflectionWalker::FindPropertyForwarding(
			Settings->GetClass(), MonolithKeyToString(Pair.Key));
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
			// ResolveNode treats these as aliases for the graph's special input
			// and output nodes. An element node carrying one of them makes
			// node_id ambiguous - connection actions select the special node
			// while element-only actions select the element - so callers can
			// mutate a different endpoint than graph info reports.
			for (const TCHAR* Reserved : { TEXT("__input__"), TEXT("__output__"), TEXT("input"), TEXT("output") })
			{
				if (Id.Equals(Reserved, ESearchCase::IgnoreCase))
				{
					AddError(FString::Printf(
						TEXT("Node '%s' uses reserved node_id '%s', which is ambiguous with the graph's special input/output nodes"),
						*Id,
						Reserved));
					break;
				}
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

void FinalizeGraphRollbackDirtyState(UPCGGraph* Graph, bool bWasDirty, bool bRollbackComplete)
{
	if (Graph && Graph->GetPackage())
	{
		// Never hide a partially restored graph by returning a previously clean
		// dirty bit. An incomplete rollback must remain visible to Save All and
		// source-control reconciliation.
		Graph->GetPackage()->SetDirtyFlag(bRollbackComplete ? bWasDirty : true);
	}
}

bool RestoreGraphUserParameters(
	UPCGGraph* Graph,
	const FInstancedPropertyBag& OriginalBag,
	bool bWasDirty)
{
	if (!Graph)
	{
		return false;
	}
	Graph->UpdateUserParametersStruct([&OriginalBag](FInstancedPropertyBag& Parameters)
	{
		Parameters = OriginalBag;
	});
	const bool bRestored =
		MonolithPCGPropertyBagUtils::AreExactlyEquivalent(
			OriginalBag,
			Graph->GetUserParametersStruct());
	FinalizeGraphRollbackDirtyState(Graph, bWasDirty, bRestored);
	return bRestored;
}

int32 GetUserParameterCount(const UPCGGraph* Graph)
{
	const FInstancedPropertyBag* Bag = Graph ? Graph->GetUserParametersStruct() : nullptr;
	const UPropertyBag* BagStruct = Bag ? Bag->GetPropertyBagStruct() : nullptr;
	return BagStruct ? BagStruct->GetPropertyDescs().Num() : 0;
}

bool BuildRelativeOuterNameClassKey(
	const UObject* Object,
	const UObject* Root,
	FString& OutKey);

class FBoundedPersistentPropertyWriter final : public FMemoryWriter
{
public:
	FBoundedPersistentPropertyWriter(TArray<uint8>& InBytes, const int64 InMaxBytes)
		: FMemoryWriter(InBytes, /*bIsPersistent=*/true)
		, MaxBytes(InMaxBytes)
	{
	}

	virtual void Serialize(void* Data, int64 Num) override
	{
		const int64 CurrentOffset = Tell();
		if (Num < 0 || CurrentOffset < 0 || CurrentOffset > MaxBytes || Num > MaxBytes - CurrentOffset)
		{
			bExceededBound = true;
			SetError();
			return;
		}
		FMemoryWriter::Serialize(Data, Num);
	}

	bool ExceededBound() const
	{
		return bExceededBound;
	}

private:
	int64 MaxBytes = 0;
	bool bExceededBound = false;
};

/**
 * Serializes one reflected property with package-persistent archive semantics
 * and without recursively serializing referenced UObjects. Persistent mode is
 * important for nested structs: UE's normal ShouldSerializeValue path then
 * excludes transient/deprecated cache fields just as package saving does.
 * Duplicate port flags are deliberately not used: DuplicateTransient controls
 * duplicate replay, but a non-Transient value can still belong to saved state.
 * References owned by the compared graph are replaced by a stable root-relative
 * outer/name/class key; external references retain their exact object path. This
 * gives corresponding source/preview/target objects the same byte representation
 * without conflating distinct external assets.
 */
class FCanonicalPersistentPropertyArchive final : public FObjectAndNameAsStringProxyArchive
{
public:
	FCanonicalPersistentPropertyArchive(
		FArchive& InInnerArchive,
		const UObject* InRoot,
		TSet<FString>& OutReferencedInnerKeys,
		FString& OutError)
		: FObjectAndNameAsStringProxyArchive(InInnerArchive, /*bInLoadIfFindFails=*/false)
		, Root(InRoot)
		, ReferencedInnerKeys(OutReferencedInnerKeys)
		, Error(OutError)
	{
	}

	virtual FArchive& operator<<(UObject*& Object) override
	{
		FString Token;
		if (!Object)
		{
			Token = TEXT("$NULL");
		}
		else if (Object == Root)
		{
			Token = TEXT("$ROOT");
		}
		else if (Root && Object->IsIn(Root))
		{
			FString RelativeKey;
			if (!BuildRelativeOuterNameClassKey(Object, Root, RelativeKey))
			{
				Error = FString::Printf(
					TEXT("could not build a bounded relative identity for graph-owned object '%s'"),
					*Object->GetPathName());
				SetError();
				Token = TEXT("$INVALID_INNER");
			}
			else
			{
				ReferencedInnerKeys.Add(RelativeKey);
				Token = TEXT("$INNER:") + RelativeKey;
			}
		}
		else
		{
			Token = TEXT("$EXTERNAL:") + Object->GetPathName();
		}
		InnerArchive << Token;
		return *this;
	}

private:
	const UObject* Root = nullptr;
	TSet<FString>& ReferencedInnerKeys;
	FString& Error;
};

bool BuildPersistentObjectIndex(
	const UPCGGraph* Root,
	TMap<FString, const UObject*>& OutObjectsByKey,
	FString& OutError)
{
	OutObjectsByKey.Reset();
	if (!Root)
	{
		OutError = TEXT("cannot index a null persistent graph root");
		return false;
	}
	OutObjectsByKey.Add(TEXT("$ROOT"), Root);

	TArray<UObject*> NestedObjects;
	GetObjectsWithOuter(
		const_cast<UPCGGraph*>(Root),
		NestedObjects,
		EGetObjectsFlags::IncludeNestedObjects);
	const int64 TotalObjectCount = static_cast<int64>(NestedObjects.Num()) + 1;
	if (TotalObjectCount > MaxPersistentGraphObjects)
	{
		OutError = FString::Printf(
			TEXT("persistent graph object count %lld exceeds comparison bound %d"),
			TotalObjectCount, MaxPersistentGraphObjects);
		return false;
	}
	OutObjectsByKey.Reserve(NestedObjects.Num() + 1);
	for (const UObject* const Object : NestedObjects)
	{
		FString RelativeKey;
		if (!BuildRelativeOuterNameClassKey(Object, Root, RelativeKey))
		{
			OutError = FString::Printf(
				TEXT("graph-owned object '%s' exceeds the relative identity depth bound %d"),
				Object ? *Object->GetPathName() : TEXT("<null>"),
				MaxPersistentGraphObjectDepth);
			return false;
		}
		if (OutObjectsByKey.Contains(RelativeKey))
		{
			OutError = FString::Printf(
				TEXT("persistent graph contains duplicate relative object identity '%s'"),
				*RelativeKey);
			return false;
		}
		OutObjectsByKey.Add(MoveTemp(RelativeKey), Object);
	}
	return true;
}

bool SerializeCanonicalPersistentProperty(
	const FProperty* Property,
	const void* Value,
	const UPCGGraph* Root,
	int64& InOutSerializedBytes,
	TSet<FString>& OutReferencedInnerKeys,
	TArray<uint8>& OutBytes,
	FString& OutError)
{
	OutBytes.Reset();
	if (!Property || !Value || !Root)
	{
		OutError = TEXT("cannot serialize a null persistent property, value, or graph root");
		return false;
	}
	if (InOutSerializedBytes > MaxPersistentGraphSerializedBytes)
	{
		OutError = FString::Printf(
			TEXT("persistent property payload exceeds comparison bound of %lld bytes"),
			MaxPersistentGraphSerializedBytes);
		return false;
	}

	const int64 RemainingBytes = MaxPersistentGraphSerializedBytes - InOutSerializedBytes;
	FBoundedPersistentPropertyWriter MemoryWriter(OutBytes, RemainingBytes);
	FCanonicalPersistentPropertyArchive CanonicalArchive(
		MemoryWriter, Root, OutReferencedInnerKeys, OutError);
	CanonicalArchive.SetIsSaving(true);
	CanonicalArchive.ArNoDelta = true;
	FStructuredArchiveFromArchive StructuredArchive(CanonicalArchive);
	Property->SerializeItem(
		StructuredArchive.GetSlot(),
		const_cast<void*>(Value),
		/*Defaults=*/nullptr);
	if (MemoryWriter.ExceededBound())
	{
		OutError = FString::Printf(
			TEXT("persistent property '%s' exceeds remaining comparison bound of %lld bytes"),
			*Property->GetName(), RemainingBytes);
		return false;
	}
	if (CanonicalArchive.IsError() || MemoryWriter.IsError())
	{
		if (OutError.IsEmpty())
		{
			OutError = FString::Printf(
				TEXT("persistent property '%s' could not be serialized canonically"),
				*Property->GetName());
		}
		return false;
	}
	InOutSerializedBytes += OutBytes.Num();
	return true;
}

bool AreUserParametersPersistentlyIdentical(
	const UPCGGraph* A,
	const UPCGGraph* B,
	int64& InOutABytes,
	int64& InOutBBytes,
	TSet<FString>& OutAReferencedInnerKeys,
	TSet<FString>& OutBReferencedInnerKeys,
	FString& OutMismatch,
	FString& OutError)
{
	OutMismatch.Reset();
	const FInstancedPropertyBag* AParameters = A ? A->GetUserParametersStruct() : nullptr;
	const FInstancedPropertyBag* BParameters = B ? B->GetUserParametersStruct() : nullptr;
	if (!AParameters || !BParameters)
	{
		if (AParameters == BParameters)
		{
			return true;
		}
		OutMismatch = TEXT("UserParameters.presence");
		return false;
	}

	const UPropertyBag* AStruct = AParameters->GetPropertyBagStruct();
	const UPropertyBag* BStruct = BParameters->GetPropertyBagStruct();
	if (!AStruct || !BStruct)
	{
		if (AStruct == BStruct)
		{
			return true;
		}
		OutMismatch = TEXT("UserParameters.schema.presence");
		return false;
	}

	const TConstArrayView<FPropertyBagPropertyDesc> ADescs = AStruct->GetPropertyDescs();
	const TConstArrayView<FPropertyBagPropertyDesc> BDescs = BStruct->GetPropertyDescs();
	if (ADescs.Num() != BDescs.Num())
	{
		OutMismatch = TEXT("UserParameters.schema.count");
		return false;
	}

	const uint8* const AMemory = AParameters->GetValue().GetMemory();
	const uint8* const BMemory = BParameters->GetValue().GetMemory();
	if (!AMemory || !BMemory)
	{
		OutMismatch = TEXT("UserParameters.value_memory");
		return false;
	}

	TArray<uint8> ABytes;
	TArray<uint8> BBytes;
	for (int32 Index = 0; Index < ADescs.Num(); ++Index)
	{
		const FPropertyBagPropertyDesc& ADesc = ADescs[Index];
		const FPropertyBagPropertyDesc& BDesc = BDescs[Index];
		const bool bSamePersistentDescriptor =
			ADesc.ValueTypeObject.Get() == BDesc.ValueTypeObject.Get() &&
			ADesc.ID == BDesc.ID &&
			ADesc.Name == BDesc.Name &&
			ADesc.ValueType == BDesc.ValueType &&
			ADesc.ContainerTypes == BDesc.ContainerTypes &&
			ADesc.PropertyFlags == BDesc.PropertyFlags
#if UE_VERSION_NEWER_THAN_OR_EQUAL(5, 8, 0)
			&&
			ADesc.KeyType == BDesc.KeyType &&
			ADesc.KeyTypeObject.Get() == BDesc.KeyTypeObject.Get()
#endif
#if WITH_EDITORONLY_DATA
			&& ADesc.MetaData == BDesc.MetaData
			&& ADesc.MetaClass.Get() == BDesc.MetaClass.Get()
#endif
			;
		if (!bSamePersistentDescriptor)
		{
			OutMismatch = FString::Printf(
				TEXT("UserParameters.schema[%d:%s]"), Index, *ADesc.Name.ToString());
			return false;
		}

		const FProperty* const AProperty = ADesc.CachedProperty;
		const FProperty* const BProperty = BDesc.CachedProperty;
		if (!AProperty || !BProperty || !AProperty->SameType(BProperty))
		{
			OutMismatch = FString::Printf(
				TEXT("UserParameters.runtime_property[%d:%s]"), Index, *ADesc.Name.ToString());
			return false;
		}

		const void* const AValue = AMemory + AProperty->GetOffset_ForInternal();
		const void* const BValue = BMemory + BProperty->GetOffset_ForInternal();
		if (!SerializeCanonicalPersistentProperty(
				AProperty, AValue, A, InOutABytes, OutAReferencedInnerKeys, ABytes, OutError) ||
			!SerializeCanonicalPersistentProperty(
				BProperty, BValue, B, InOutBBytes, OutBReferencedInnerKeys, BBytes, OutError))
		{
			return false;
		}
		if (ABytes != BBytes)
		{
			OutMismatch = FString::Printf(
				TEXT("UserParameters.value[%d:%s]"), Index, *ADesc.Name.ToString());
			return false;
		}
	}

	return true;
}

bool ArePersistentGraphPropertiesIdentical(
	const UPCGGraph* A,
	const UPCGGraph* B,
	FString& OutMismatch,
	FString& OutError)
{
	OutMismatch.Reset();
	OutError.Reset();
	if (!A || !B || A->GetClass() != B->GetClass())
	{
		OutMismatch = TEXT("graph classes differ");
		return false;
	}

	TMap<FString, const UObject*> AObjectsByKey;
	TMap<FString, const UObject*> BObjectsByKey;
	if (!BuildPersistentObjectIndex(A, AObjectsByKey, OutError) ||
		!BuildPersistentObjectIndex(B, BObjectsByKey, OutError))
	{
		return false;
	}

	const FProperty* const UserParametersProperty =
		FindFProperty<FProperty>(A->GetClass(), TEXT("UserParameters"));
	if (!UserParametersProperty)
	{
		OutError = TEXT("UPCGGraph has no reflected UserParameters property");
		return false;
	}

	int64 ASerializedBytes = 0;
	int64 BSerializedBytes = 0;
	TArray<FString> PendingKeys;
	PendingKeys.Add(TEXT("$ROOT"));
	TSet<FString> QueuedKeys;
	QueuedKeys.Add(TEXT("$ROOT"));
	TSet<FString> AReferencedInnerKeys;
	TSet<FString> BReferencedInnerKeys;
	TArray<FString> SortedReferencedKeys;
	TArray<uint8> ABytes;
	TArray<uint8> BBytes;
	for (int32 PendingIndex = 0; PendingIndex < PendingKeys.Num(); ++PendingIndex)
	{
		if (PendingKeys.Num() > MaxPersistentGraphObjects)
		{
			OutError = FString::Printf(
				TEXT("reachable persistent graph object count exceeds comparison bound %d"),
				MaxPersistentGraphObjects);
			return false;
		}

		const FString ObjectKey = PendingKeys[PendingIndex];
		const UObject* const* AObjectPtr = AObjectsByKey.Find(ObjectKey);
		const UObject* const* BObjectPtr = BObjectsByKey.Find(ObjectKey);
		if (!AObjectPtr || !BObjectPtr || !*AObjectPtr || !*BObjectPtr)
		{
			OutMismatch = ObjectKey + TEXT(".presence");
			return false;
		}
		const UObject* const AObject = *AObjectPtr;
		const UObject* const BObject = *BObjectPtr;
		if (AObject->GetClass() != BObject->GetClass())
		{
			OutMismatch = ObjectKey + TEXT(".class");
			return false;
		}

		AReferencedInnerKeys.Reset();
		BReferencedInnerKeys.Reset();
		if (PendingIndex == 0 &&
			!AreUserParametersPersistentlyIdentical(
				A, B,
				ASerializedBytes, BSerializedBytes,
				AReferencedInnerKeys, BReferencedInnerKeys,
				OutMismatch, OutError))
		{
			return false;
		}

		for (TFieldIterator<FProperty> It(
				AObject->GetClass(), EFieldIteratorFlags::IncludeSuper); It; ++It)
		{
			const FProperty* const Property = *It;
#if WITH_EDITORONLY_DATA && UE_VERSION_NEWER_THAN_OR_EQUAL(5, 8, 0)
			const bool bIdentityBoundEditorWorkspaceState =
				PendingIndex == 0
				&& Property
				&& Property->GetFName() ==
					GET_MEMBER_NAME_CHECKED(UPCGGraphInterface, LastEditedDocuments);
#else
			const bool bIdentityBoundEditorWorkspaceState = false;
#endif
			if (!Property || !Property->ShouldDuplicateValue() ||
				(PendingIndex == 0 && Property == UserParametersProperty) ||
				bIdentityBoundEditorWorkspaceState)
			{
				continue;
			}
			for (int32 ArrayIndex = 0; ArrayIndex < Property->ArrayDim; ++ArrayIndex)
			{
				const void* const AValue = Property->ContainerPtrToValuePtr<void>(AObject, ArrayIndex);
				const void* const BValue = Property->ContainerPtrToValuePtr<void>(BObject, ArrayIndex);
				if (!SerializeCanonicalPersistentProperty(
						Property, AValue, A, ASerializedBytes,
						AReferencedInnerKeys, ABytes, OutError) ||
					!SerializeCanonicalPersistentProperty(
						Property, BValue, B, BSerializedBytes,
						BReferencedInnerKeys, BBytes, OutError))
				{
					return false;
				}
				if (ABytes != BBytes)
				{
					OutMismatch = ObjectKey == TEXT("$ROOT")
						? Property->GetName()
						: ObjectKey + TEXT(".") + Property->GetName();
					return false;
				}
			}
		}

		// Preserve the native/intrinsic comparison hook used by CoreUObject's
		// deep comparator. Current PCG graph-owned classes use the UObject default,
		// but this keeps the walker correct for a future graph inner that overrides it.
		if (PendingIndex > 0 &&
			!AObject->AreNativePropertiesIdenticalTo(const_cast<UObject*>(BObject)))
		{
			OutMismatch = ObjectKey + TEXT(".native_properties");
			return false;
		}

		if (AReferencedInnerKeys.Num() != BReferencedInnerKeys.Num())
		{
			OutMismatch = ObjectKey + TEXT(".referenced_inner_count");
			return false;
		}
		SortedReferencedKeys.Reset(AReferencedInnerKeys.Num());
		for (const FString& ReferencedKey : AReferencedInnerKeys)
		{
			SortedReferencedKeys.Add(ReferencedKey);
		}
		SortedReferencedKeys.Sort();
		for (const FString& ReferencedKey : SortedReferencedKeys)
		{
			if (!BReferencedInnerKeys.Contains(ReferencedKey))
			{
				OutMismatch = ObjectKey + TEXT(".referenced_inner_identity");
				return false;
			}
			if (!QueuedKeys.Contains(ReferencedKey))
			{
				QueuedKeys.Add(ReferencedKey);
				PendingKeys.Add(ReferencedKey);
			}
		}
	}
	return true;
}

bool PrepareGraphReplacementSourceControl(
	UPCGGraph* Target,
	TSharedPtr<FJsonObject>& OutPrepare,
	FMonolithActionResult& OutError)
{
	OutPrepare.Reset();
#if WITH_DEV_AUTOMATION_TESTS
	if (Target && UE::MonolithPCG::Private::TryBuildGraphContentsReplacementTestSourceControl(
			Target->GetPathName(), OutPrepare))
	{
		return true;
	}
#endif
	FMonolithSourceControlPrepareOptions Options;
	Options.bUnavailableIsSuccess = true;
	TSharedPtr<FJsonObject> BeforeAction =
		FMonolithSourceControlUtils::CheckoutOrAddPackage(Target ? Target->GetPackage() : nullptr, Options);
	bool bOk = false;
	bool bAvailable = false;
	if (BeforeAction.IsValid())
	{
		BeforeAction->TryGetBoolField(TEXT("ok"), bOk);
		BeforeAction->TryGetBoolField(TEXT("available"), bAvailable);
	}
	else
	{
		BeforeAction = MakeShared<FJsonObject>();
		BeforeAction->SetBoolField(TEXT("ok"), false);
		BeforeAction->SetStringField(TEXT("status"), TEXT("missing_before_action"));
		BeforeAction->SetStringField(
			TEXT("message"), TEXT("Source-control utility returned no result"));
	}

	OutPrepare = MakeShared<FJsonObject>();
	OutPrepare->SetStringField(TEXT("mode"), TEXT("handler_owned_pre_mutation"));
	OutPrepare->SetStringField(
		TEXT("status"), !bOk ? TEXT("failed") :
			(bAvailable ? TEXT("prepared") : TEXT("skipped_provider_unavailable")));
	OutPrepare->SetObjectField(TEXT("before_action"), BeforeAction);
	if (bOk)
	{
		return true;
	}

	TSharedPtr<FJsonObject> ErrorData = MakeShared<FJsonObject>();
	ErrorData->SetObjectField(TEXT("source_control_prepare"), OutPrepare);
	OutError = FMonolithActionResult::Error(
		TEXT("replace_pcg_graph_contents aborted before mutation because source-control preparation failed"))
		.WithErrorData(ErrorData);
	return false;
}

FMonolithActionResult AttachGraphReplacementSourceControl(
	FMonolithActionResult Result,
	const TSharedPtr<FJsonObject>& Prepare)
{
	if (!Prepare.IsValid())
	{
		return Result;
	}
	if (Result.bSuccess && Result.Result.IsValid())
	{
		Result.Result->SetObjectField(TEXT("source_control_prepare"), Prepare);
	}
	else
	{
		MonolithPCGResultUtils::EnsureErrorDataObject(Result)->SetObjectField(
			TEXT("source_control_prepare"), Prepare);
	}
	return Result;
}

void PrepareGraphForUndo(UPCGGraph* Graph)
{
	if (!Graph)
	{
		return;
	}
	Graph->Modify();
	TArray<UObject*> NestedObjects;
	GetObjectsWithOuter(Graph, NestedObjects, EGetObjectsFlags::IncludeNestedObjects);
	for (UObject* Object : NestedObjects)
	{
		if (Object && Object->HasAnyFlags(RF_Transactional))
		{
			Object->Modify();
		}
	}
}

void RebindGraphNodeDelegates(UPCGGraph* Graph)
{
#if WITH_EDITOR
	if (!Graph)
	{
		return;
	}
	auto Rebind = [Graph](UPCGNode* Node)
	{
		if (Node)
		{
			Graph->PreNodeUndo(Node);
			Graph->PostNodeUndo(Node);
		}
	};
	Rebind(Graph->GetInputNode());
	Rebind(Graph->GetOutputNode());
	for (UPCGNode* Node : Graph->GetNodes())
	{
		Rebind(Node);
	}
#endif
}

void DiscardTransientGraph(UPCGGraph*& Graph)
{
	if (!Graph)
	{
		return;
	}
	Graph->ClearFlags(RF_Public | RF_Standalone);
	Graph->MarkAsGarbage();
	Graph = nullptr;
}

UPCGGraph* DuplicateGraphToTransient(const UPCGGraph* Graph, const TCHAR* BaseName, FString& OutError)
{
	if (!Graph)
	{
		OutError = TEXT("cannot duplicate a null PCG graph");
		return nullptr;
	}
	const FName UniqueName = MakeUniqueObjectName(
		GetTransientPackage(), UPCGGraph::StaticClass(), FName(BaseName));
	UPCGGraph* Duplicate = DuplicateObject<UPCGGraph>(Graph, GetTransientPackage(), UniqueName);
	if (!Duplicate || Duplicate->GetClass() != Graph->GetClass())
	{
		OutError = TEXT("failed to create an exact transient UPCGGraph snapshot");
		return nullptr;
	}
	return Duplicate;
}

bool ValidateDuplicablePropertyArchetype(const UObject* Object, FString& OutError)
{
	if (!Object)
	{
		OutError = TEXT("cannot reset a null seeded-duplication destination");
		return false;
	}

	const UObject* const Archetype = Object->GetArchetype();
	if (!Archetype || Archetype == Object || Archetype->GetClass() != Object->GetClass())
	{
		OutError = FString::Printf(
			TEXT("seeded-duplication destination '%s' has no exact class archetype baseline"),
			*Object->GetPathName());
		return false;
	}
	return true;
}

void ResetDuplicablePropertiesToArchetype(UObject* Object)
{
	check(Object && Object->GetArchetype() && Object->GetArchetype() != Object);
	UObject* const Archetype = Object->GetArchetype();
	for (TFieldIterator<FProperty> It(Object->GetClass(), EFieldIteratorFlags::IncludeSuper); It; ++It)
	{
		FProperty* const Property = *It;
		if (Property && Property->ShouldDuplicateValue())
		{
			Property->CopyCompleteValue_InContainer(Object, Archetype);
		}
	}
}

bool BuildRelativeOuterNameClassKey(
	const UObject* Object,
	const UObject* Root,
	FString& OutKey)
{
	OutKey.Reset();
	if (!Object || !Root || Object == Root)
	{
		return false;
	}

	TArray<const UObject*> RelativeChain;
	for (const UObject* Cursor = Object; Cursor && Cursor != Root; Cursor = Cursor->GetOuter())
	{
		if (RelativeChain.Num() >= MaxPersistentGraphObjectDepth)
		{
			return false;
		}
		RelativeChain.Add(Cursor);
	}
	if (RelativeChain.IsEmpty() || RelativeChain.Last()->GetOuter() != Root)
	{
		return false;
	}

	// Encode every ancestor's exact name and class with length prefixes. This
	// preserves the old recursive contract: a same-name leaf is reusable only
	// when every parent in its source-relative lineage also matches exactly.
	for (int32 Index = RelativeChain.Num() - 1; Index >= 0; --Index)
	{
		const UObject* const Segment = RelativeChain[Index];
		const FString Name = Segment->GetFName().ToString();
		const FString ClassPath = Segment->GetClass()->GetPathName();
		OutKey += FString::Printf(
			TEXT("%d:%s%d:%s"),
			Name.Len(), *Name,
			ClassPath.Len(), *ClassPath);
	}
	return true;
}

void CollectReusableSeededDuplicationDestinations(
	const UObject* Source,
	UObject* Target,
	TArray<UObject*>& OutTargets)
{
	OutTargets.Reset();
	if (!Source || !Target)
	{
		return;
	}
	OutTargets.Add(Target);

	TArray<UObject*> TargetObjects;
	GetObjectsWithOuter(Target, TargetObjects, EGetObjectsFlags::IncludeNestedObjects);
	TMap<FString, UObject*> TargetsByRelativeLineage;
	TargetsByRelativeLineage.Reserve(TargetObjects.Num());
	for (UObject* const TargetObject : TargetObjects)
	{
		FString RelativeKey;
		if (BuildRelativeOuterNameClassKey(TargetObject, Target, RelativeKey))
		{
			TargetsByRelativeLineage.Add(MoveTemp(RelativeKey), TargetObject);
		}
	}

	TArray<UObject*> SourceObjects;
	GetObjectsWithOuter(Source, SourceObjects, EGetObjectsFlags::IncludeNestedObjects);
	OutTargets.Reserve(FMath::Min(SourceObjects.Num(), TargetObjects.Num()) + 1);
	for (const UObject* const SourceObject : SourceObjects)
	{
		FString RelativeKey;
		if (!BuildRelativeOuterNameClassKey(SourceObject, Source, RelativeKey))
		{
			continue;
		}
		if (UObject* const* ReusableTarget = TargetsByRelativeLineage.Find(RelativeKey))
		{
			OutTargets.Add(*ReusableTarget);
		}
	}
}

bool ResetSeededDuplicationDestinationBaselines(
	const UPCGGraph* Source,
	UPCGGraph* Target,
	FString& OutError)
{
	if (!Source || !Target)
	{
		OutError = TEXT("cannot reset a null PCG graph source or destination");
		return false;
	}

	// StaticDuplicateObjectEx normally constructs its destination from the class
	// archetype before applying the source's delta-serialized values. A root in
	// DuplicationSeed is deliberately reused instead, as are any same-name inner
	// objects that the writer maps onto the existing graph. Without recreating
	// that archetype baseline first, a source value equal to its default is
	// omitted from the duplicate stream and the previous target value survives.
	// Reset exactly the source-relative objects that the duplicate writer can
	// reuse by outer/name/class. Unmatched target-only editor objects are not
	// touched; unmatched persistent graph objects are removed by the replacement.
	TArray<UObject*> ExistingObjects;
	CollectReusableSeededDuplicationDestinations(Source, Target, ExistingObjects);

	// Preflight the entire tree before the first property is reset so this
	// preparation step cannot fail after a partial mutation.
	for (const UObject* const Object : ExistingObjects)
	{
		if (!ValidateDuplicablePropertyArchetype(Object, OutError))
		{
			return false;
		}
	}
	for (UObject* const Object : ExistingObjects)
	{
		ResetDuplicablePropertiesToArchetype(Object);
	}
	return true;
}

bool ReplaceGraphObjectState(
	const UPCGGraph* Source,
	UPCGGraph* Target,
	bool bTransactionalAndNotify,
	FString& OutError)
{
	OutError.Reset();
	if (!Source || !Target || Source == Target ||
		Source->GetClass() != UPCGGraph::StaticClass() ||
		Target->GetClass() != UPCGGraph::StaticClass())
	{
		OutError = TEXT("graph state replacement requires distinct exact UPCGGraph objects");
		return false;
	}

	const FString TargetPathBefore = Target->GetPathName();
	const FName TargetNameBefore = Target->GetFName();
	UObject* const TargetOuterBefore = Target->GetOuter();
	UPCGNode* const TargetInputNodeBefore = Target->GetInputNode();
	UPCGNode* const TargetOutputNodeBefore = Target->GetOutputNode();
	UPCGSettings* const TargetInputSettingsBefore =
		TargetInputNodeBefore ? TargetInputNodeBefore->GetSettings() : nullptr;
	UPCGSettings* const TargetOutputSettingsBefore =
		TargetOutputNodeBefore ? TargetOutputNodeBefore->GetSettings() : nullptr;
#if WITH_EDITORONLY_DATA && UE_VERSION_NEWER_THAN_OR_EQUAL(5, 8, 0)
	// LastEditedDocuments is persisted by PCG solely to restore the target
	// asset's editor workspace.  It is identity-bound UI state, not graph
	// contents: importing the donor's soft paths would point the canonical
	// target back at the donor package.  Preserve it across both preview and
	// commit replacement, while the deep comparator intentionally ignores it.
	const TArray<FPCGGraphDocumentInfo> TargetLastEditedDocumentsBefore =
		Target->LastEditedDocuments;
#endif
	TArray<TWeakObjectPtr<UPCGNode>> PreviousElementNodes;
	PreviousElementNodes.Reserve(Target->GetNodes().Num());
	for (UPCGNode* Node : Target->GetNodes())
	{
		PreviousElementNodes.Add(Node);
	}

	auto DuplicateIntoExistingTarget = [&]() -> bool
	{
		if (bTransactionalAndNotify)
		{
			PrepareGraphForUndo(Target);
		}
		if (!ResetSeededDuplicationDestinationBaselines(Source, Target, OutError))
		{
			return false;
		}

		FObjectDuplicationParameters DuplicationParameters(
			const_cast<UPCGGraph*>(Source), TargetOuterBefore);
		DuplicationParameters.DestName = TargetNameBefore;
		DuplicationParameters.DestClass = Target->GetClass();
		DuplicationParameters.bAssignExternalPackages = false;
		DuplicationParameters.DuplicationSeed.Add(const_cast<UPCGGraph*>(Source), Target);
		TMap<UObject*, UObject*> CreatedObjects;
		DuplicationParameters.CreatedObjects = &CreatedObjects;
		if (StaticDuplicateObjectEx(DuplicationParameters) != Target)
		{
			return false;
		}
#if WITH_EDITORONLY_DATA && UE_VERSION_NEWER_THAN_OR_EQUAL(5, 8, 0)
		Target->LastEditedDocuments = TargetLastEditedDocumentsBefore;
#endif
		return true;
	};

	if (bTransactionalAndNotify)
	{
		FMonolithPCGScopedGraphEditNotifications NotificationScope(Target);
		if (!DuplicateIntoExistingTarget())
		{
			if (OutError.IsEmpty())
			{
				OutError = TEXT("StaticDuplicateObjectEx did not reuse the exact target UPCGGraph identity");
			}
			return false;
		}
		RebindGraphNodeDelegates(Target);
		Target->ForceNotificationForEditor(
			EPCGChangeType::Structural | EPCGChangeType::Settings |
			EPCGChangeType::Edge | EPCGChangeType::Input |
			EPCGChangeType::GenerationGrid);
		NotificationScope.MarkExternalModification();
	}
	else if (!DuplicateIntoExistingTarget())
	{
		if (OutError.IsEmpty())
		{
			OutError = TEXT("StaticDuplicateObjectEx did not reuse the exact preview UPCGGraph identity");
		}
		return false;
	}
	if (bTransactionalAndNotify)
	{
		// Seeded duplication may reuse an element node when names match. Mirror
		// UPCGGraph::RemoveNodes_Internal for old nodes no longer referenced by the
		// graph: move them to the transient package but keep them alive so the
		// surrounding editor transaction can restore them on Undo.
		for (const TWeakObjectPtr<UPCGNode>& PreviousNode : PreviousElementNodes)
		{
			UPCGNode* Node = PreviousNode.Get();
			if (Node && !Target->Contains(Node) && Node->GetOuter() == Target)
			{
				Node->Rename(
					nullptr,
					GetTransientPackage(),
					REN_DontCreateRedirectors | REN_AllowPackageLinkerMismatch);
			}
		}
	}

	if (Target->GetOuter() != TargetOuterBefore || Target->GetFName() != TargetNameBefore ||
		!Target->GetPathName().Equals(TargetPathBefore, ESearchCase::CaseSensitive))
	{
		OutError = TEXT("graph state replacement changed the target package/object identity");
		return false;
	}

	for (UPCGNode* Node : Target->GetNodes())
	{
		if (!Node || Node->GetGraph() != Target || !Target->Contains(Node))
		{
			OutError = TEXT("graph state replacement produced an element node outside the target graph");
			return false;
		}
	}
	if (!Target->GetInputNode() || !Target->GetOutputNode() ||
		Target->GetInputNode()->GetGraph() != Target ||
		Target->GetOutputNode()->GetGraph() != Target)
	{
		OutError = TEXT("graph state replacement did not preserve target-owned input/output nodes");
		return false;
	}
	if (Target->GetInputNode() != TargetInputNodeBefore ||
		Target->GetOutputNode() != TargetOutputNodeBefore ||
		Target->GetInputNode()->GetSettings() != TargetInputSettingsBefore ||
		Target->GetOutputNode()->GetSettings() != TargetOutputSettingsBefore)
	{
		OutError = TEXT(
			"graph state replacement did not reuse the target's default input/output nodes and settings");
		return false;
	}

	FString Mismatch;
	FString ComparisonError;
	if (!ArePersistentGraphPropertiesIdentical(Source, Target, Mismatch, ComparisonError))
	{
		OutError = !ComparisonError.IsEmpty()
			? FString::Printf(
				TEXT("graph state replacement could not verify persistent properties: %s"),
				*ComparisonError)
			: FString::Printf(
				TEXT("graph state replacement failed exact persistent-property read-back at '%s'"),
				*Mismatch);
		return false;
	}

	return true;
}

bool RestoreGraphFromSnapshot(
	UPCGGraph* Target,
	const UPCGGraph* Snapshot,
	bool bWasDirty,
	FString& OutError)
{
	if (!ReplaceGraphObjectState(Snapshot, Target, true, OutError))
	{
		if (Target && Target->GetPackage())
		{
			Target->GetPackage()->SetDirtyFlag(true);
		}
		return false;
	}
	RestorePackageDirtyState(Target, bWasDirty);
	FString Mismatch;
	FString ComparisonError;
	if (!ArePersistentGraphPropertiesIdentical(Snapshot, Target, Mismatch, ComparisonError))
	{
		OutError = !ComparisonError.IsEmpty()
			? FString::Printf(
				TEXT("rollback persistent-property verification could not complete: %s"),
				*ComparisonError)
			: FString::Printf(
				TEXT("rollback verification failed at persistent property '%s'"), *Mismatch);
		Target->GetPackage()->SetDirtyFlag(true);
		return false;
	}
	if (Target->GetPackage()->IsDirty() != bWasDirty)
	{
		OutError = TEXT("rollback could not restore the original package dirty state");
		Target->GetPackage()->SetDirtyFlag(true);
		return false;
	}
	return true;
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

bool AreEdgeDescriptorsEqual(
	const TArray<FGraphEdgeDescriptor>& A,
	const TArray<FGraphEdgeDescriptor>& B)
{
	if (A.Num() != B.Num())
	{
		return false;
	}
	for (int32 Index = 0; Index < A.Num(); ++Index)
	{
		if (A[Index].SourceNode != B[Index].SourceNode ||
			A[Index].SourcePin != B[Index].SourcePin ||
			A[Index].TargetNode != B[Index].TargetNode ||
			A[Index].TargetPin != B[Index].TargetPin)
		{
			return false;
		}
	}
	return true;
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

bool RollbackConnectedEdge(UPCGGraph* Graph, UPCGNode* SourceNode, const FName SourcePin, UPCGNode* TargetNode,
						   const FName TargetPin)
{
	if (!Graph || !SourceNode || !TargetNode)
	{
		return false;
	}
	FMonolithPCGScopedGraphEditNotifications NotificationScope(Graph);
	Graph->RemoveEdge(SourceNode, SourcePin, TargetNode, TargetPin);
	NotificationScope.MarkExternalModification();
	return !AreConnected(SourceNode->GetOutputPin(SourcePin), TargetNode->GetInputPin(TargetPin));
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
	bRestored &= AreEdgeDescriptorsEqual(CaptureIncidentEdges(Graph, Node), OriginalIncidentEdges);
	FinalizeGraphRollbackDirtyState(Graph, bWasDirty, bRestored);
	return bRestored;
}

bool RestoreSubgraphMutation(UPCGGraph* Graph, UPCGNode* Node, UPCGSubgraphSettings* Settings,
							 UPCGGraphInterface* PreviousInterface,
							 const TArray<FGraphEdgeDescriptor>& OriginalIncidentEdges, bool bWasDirty)
{
	if (!Graph || !Node || !Settings || !Settings->SubgraphInstance)
	{
		return false;
	}

	bool bRestored = true;
	{
		FMonolithPCGScopedGraphEditNotifications NotificationScope(Graph);
		Settings->Modify();
		Settings->SetSubgraph(PreviousInterface);
		bRestored &= Settings->SubgraphInstance->Graph.Get() == PreviousInterface;

		for (const FGraphEdgeDescriptor& CurrentEdge : CaptureIncidentEdges(Graph, Node))
		{
			if (!CurrentEdge.SourceNode || !CurrentEdge.TargetNode)
			{
				bRestored = false;
				continue;
			}
			Graph->RemoveEdge(CurrentEdge.SourceNode, CurrentEdge.SourcePin, CurrentEdge.TargetNode,
				CurrentEdge.TargetPin);
		}

		for (const FGraphEdgeDescriptor& OriginalEdge : OriginalIncidentEdges)
		{
			if (!OriginalEdge.SourceNode || !OriginalEdge.TargetNode || OriginalEdge.SourcePin.IsNone() ||
				OriginalEdge.TargetPin.IsNone())
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

	bRestored &= AreEdgeDescriptorsEqual(CaptureIncidentEdges(Graph, Node), OriginalIncidentEdges);
	FinalizeGraphRollbackDirtyState(Graph, bWasDirty, bRestored);
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
			.StrictComplexTypes()
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
			.StrictComplexTypes()
			.Required(TEXT("asset_path"), TEXT("string"), TEXT("Project-owned PCG graph package path"))
			.Optional(TEXT("existing_policy"), TEXT("string"),
					  TEXT("Existing destination policy: fail or return_existing"), TEXT("fail"))
			.Optional(TEXT("save"), TEXT("bool"), TEXT("Persist the new graph package"), TEXT("true"))
			.Build(),
		TEXT("Graph Authoring"));

	Registry.RegisterAction(
		TEXT("pcg"), TEXT("get_pcg_graph_info"),
		TEXT("Read a PCG graph's element nodes, special input/output nodes, "
			 "pins, edges, positions, settings classes, and optional bounded "
			 "settings values."),
		FMonolithActionHandler::CreateStatic(&GetGraphInfo),
		FParamSchemaBuilder()
			.StrictComplexTypes()
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
			.StrictComplexTypes()
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
		TEXT("Graph Authoring"));

	Registry.RegisterAction(
		TEXT("pcg"), TEXT("remove_pcg_node"),
		TEXT("Remove one element node and all of its incident edges from a PCG "
			 "graph. Graph input/output nodes are protected."),
		FMonolithActionHandler::CreateStatic(&RemoveNode),
		FParamSchemaBuilder()
			.StrictComplexTypes()
			.Required(TEXT("asset_path"), TEXT("string"), TEXT("Project-owned PCG graph package or object path"))
			.Required(TEXT("node_id"), TEXT("string"),
					  TEXT("Exact node_id returned by add/get, or a unique "
						   "authored title"))
			.Optional(TEXT("save"), TEXT("bool"), TEXT("Persist the graph after mutation"), TEXT("true"))
			.Build(),
		TEXT("Graph Authoring"));

	Registry.RegisterAction(
		TEXT("pcg"), TEXT("connect_pcg_nodes"),
		TEXT("Connect an existing source output pin to a target input pin after "
			 "graph ownership, direction, direct PCG data-type compatibility, "
			 "acyclicity, and input-capacity checks; repeated calls are "
			 "idempotent."),
		FMonolithActionHandler::CreateStatic(&ConnectNodes),
		FParamSchemaBuilder()
			.StrictComplexTypes()
			.Required(TEXT("asset_path"), TEXT("string"), TEXT("Project-owned PCG graph package or object path"))
			.Required(TEXT("source_node"), TEXT("string"),
					  TEXT("Source node_id; __input__ addresses the graph input node"))
			.Required(TEXT("source_pin"), TEXT("string"), TEXT("Source output pin label"))
			.Required(TEXT("target_node"), TEXT("string"),
					  TEXT("Target node_id; __output__ addresses the graph output node"))
			.Required(TEXT("target_pin"), TEXT("string"), TEXT("Target input pin label"))
			.Optional(TEXT("save"), TEXT("bool"), TEXT("Persist the graph after mutation"), TEXT("true"))
			.Build(),
		TEXT("Graph Authoring"));

	Registry.RegisterAction(
		TEXT("pcg"), TEXT("disconnect_pcg_nodes"),
		TEXT("Disconnect one exact PCG edge. A missing edge is an idempotent "
			 "success and is reported as not_connected."),
		FMonolithActionHandler::CreateStatic(&DisconnectNodes),
		FParamSchemaBuilder()
			.StrictComplexTypes()
			.Required(TEXT("asset_path"), TEXT("string"), TEXT("Project-owned PCG graph package or object path"))
			.Required(TEXT("source_node"), TEXT("string"),
					  TEXT("Source node_id; __input__ addresses the graph input node"))
			.Required(TEXT("source_pin"), TEXT("string"), TEXT("Source output pin label"))
			.Required(TEXT("target_node"), TEXT("string"),
					  TEXT("Target node_id; __output__ addresses the graph output node"))
			.Required(TEXT("target_pin"), TEXT("string"), TEXT("Target input pin label"))
			.Optional(TEXT("save"), TEXT("bool"), TEXT("Persist the graph after mutation"), TEXT("true"))
			.Build(),
		TEXT("Graph Authoring"));

	Registry.RegisterAction(
		TEXT("pcg"), TEXT("set_pcg_node_params"),
		TEXT("Validate and apply a strict JSON property tree to one UPCGSettings "
			 "object through Monolith's canonical reflection walker and "
			 "property-chain editor callbacks. Supports side-effect-free staged "
			 "dry-run."),
		FMonolithActionHandler::CreateStatic(&SetNodeParams),
		FParamSchemaBuilder()
			.StrictComplexTypes()
			.Required(TEXT("asset_path"), TEXT("string"), TEXT("Project-owned PCG graph package or object path"))
			.Required(TEXT("node_id"), TEXT("string"),
					  TEXT("Exact node_id returned by add/get, or a unique "
						   "authored title"))
			.Required(TEXT("properties"), TEXT("object"), TEXT("Editable UPCGSettings property tree"))
			.Optional(TEXT("dry_run"), TEXT("bool"), TEXT("Validate and report without mutation"), TEXT("false"))
			.Optional(TEXT("save"), TEXT("bool"), TEXT("Persist the graph after mutation"), TEXT("true"))
			.Build(),
		TEXT("Graph Authoring"));

	Registry.RegisterAction(
		TEXT("pcg"), TEXT("set_pcg_subgraph"),
		TEXT("Assign one exact project-owned UPCGGraph or UPCGGraphInstance to an existing "
			 "UPCGSubgraphSettings node through UE's SetSubgraph contract. Supports side-effect-free "
			 "dry-run, recursion/filter guards, exact read-back, incident-topology preservation, "
			 "rollback, and optional save."),
		FMonolithActionHandler::CreateStatic(&SetSubgraph),
		FParamSchemaBuilder()
			.StrictComplexTypes()
			.Required(TEXT("asset_path"), TEXT("string"), TEXT("Project-owned parent PCG graph package or object path"))
			.Required(TEXT("node_id"), TEXT("string"),
				TEXT("Exact UPCGSubgraphSettings node_id returned by add/get, or a unique authored title"))
			.Required(TEXT("subgraph_asset_path"), TEXT("string"),
				TEXT("Exact project-owned UPCGGraph or UPCGGraphInstance package or object path"))
			.Optional(TEXT("dry_run"), TEXT("bool"), TEXT("Validate and report without mutation"), TEXT("false"))
			.Optional(TEXT("save"), TEXT("bool"), TEXT("Persist the parent graph after mutation"), TEXT("true"))
			.Build(),
		TEXT("Graph Authoring"));

	Registry.RegisterAction(
		TEXT("pcg"), TEXT("set_pcg_graph_user_parameters"),
		TEXT("Atomically add, update, type-change, or remove bounded scalar user parameters on an exact "
			 "project-owned UPCGGraph. Every upsert requires an explicit default_value; dry-run defaults true."),
		FMonolithActionHandler::CreateStatic(&SetGraphUserParameters),
		FParamSchemaBuilder()
			.StrictComplexTypes()
			.Required(TEXT("asset_path"), TEXT("string"), TEXT("Exact project-owned PCG graph package or object path"))
			.Optional(TEXT("upsert"), TEXT("array"),
				TEXT("Parameter objects with name, type, and explicit default_value"))
			.Optional(TEXT("remove"), TEXT("array"), TEXT("Exact existing parameter names to remove"))
			.Optional(TEXT("dry_run"), TEXT("bool"), TEXT("Validate and preview the atomic schema/value change"),
				TEXT("true"))
			.Optional(TEXT("save"), TEXT("bool"), TEXT("Persist the graph after mutation"), TEXT("true"))
			.Build(),
		TEXT("Graph Authoring"));

	Registry.RegisterAction(
		TEXT("pcg"), TEXT("replace_pcg_graph_contents"),
		TEXT("Replace an existing target UPCGGraph's complete persistent object graph from a distinct "
			 "exact project-owned source while preserving the target package/object identity. Copies graph-level "
			 "properties, user parameters, input/output settings and pins, element settings/editor properties, "
			 "embedded subobjects, and exact edges through CoreUObject seeded duplication. Recursive donors and "
			 "donors containing the target fail closed. Dry-run defaults true; commit requires confirm=true."),
		FMonolithActionHandler::CreateStatic(&ReplaceGraphContents),
		FParamSchemaBuilder()
			.StrictComplexTypes()
			.Required(TEXT("source_asset_path"), TEXT("string"),
				TEXT("Exact project-owned source UPCGGraph package or object path"))
			.Required(TEXT("target_asset_path"), TEXT("string"),
				TEXT("Exact existing project-owned target UPCGGraph package or object path whose identity is preserved"))
			.Optional(TEXT("dry_run"), TEXT("bool"),
				TEXT("Build and verify a transient replacement preview without mutating the target"), TEXT("true"))
			.Optional(TEXT("confirm"), TEXT("bool"),
				TEXT("Required true when dry_run=false because target contents are replaced"), TEXT("false"))
			.Optional(TEXT("save"), TEXT("bool"), TEXT("Persist the replaced target graph"), TEXT("true"))
			.Optional(TEXT("node_limit"), TEXT("integer"),
				TEXT("Maximum source or target element nodes accepted (1-5000)"), TEXT("5000"))
			.Optional(TEXT("edge_limit"), TEXT("integer"),
				TEXT("Maximum source or target edges accepted (1-20000)"), TEXT("20000"))
			.Build(),
		TEXT("Graph Authoring"));

	Registry.RegisterAction(
		TEXT("pcg"), TEXT("validate_pcg_graph"),
		TEXT("Validate PCG graph node ownership, settings, edge "
			 "endpoints/directions/types/capacity, duplicate ids/edges, directed "
			 "cycles, isolated nodes, and optional output connectivity without "
			 "mutation."),
		FMonolithActionHandler::CreateStatic(&ValidateGraph),
		FParamSchemaBuilder()
			.StrictComplexTypes()
			.Required(TEXT("asset_path"), TEXT("string"), TEXT("Project-owned PCG graph package or object path"))
			.Optional(TEXT("require_output_connection"), TEXT("bool"),
					  TEXT("Treat an unconnected graph output as an error"), TEXT("false"))
			.Optional(TEXT("require_no_isolated_nodes"), TEXT("bool"),
					  TEXT("Treat isolated element nodes as errors instead of warnings"), TEXT("false"))
			.Optional(TEXT("issue_limit"), TEXT("integer"),
					  TEXT("Maximum errors and warnings returned per array (1-1000)"), TEXT("200"))
			.Build(),
		TEXT("Graph Authoring"));

}

FMonolithActionResult FMonolithPCGGraphAuthoringActions::ListNodeTypes(const TSharedPtr<FJsonObject>& Params)
{
	FString Query;
	const TSharedPtr<FJsonValue> QueryField = Params.IsValid() ? Params->TryGetField(TEXT("query")) : nullptr;
	if (QueryField.IsValid() && QueryField->Type != EJson::Null &&
		(QueryField->Type != EJson::String || !QueryField->TryGetString(Query)))
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
	const TSharedPtr<FJsonValue> ExistingPolicyField = Params.IsValid()
		? Params->TryGetField(TEXT("existing_policy"))
		: nullptr;
	if (ExistingPolicyField.IsValid() && ExistingPolicyField->Type != EJson::Null &&
		(ExistingPolicyField->Type != EJson::String || !ExistingPolicyField->TryGetString(ExistingPolicy)))
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
			// LoadGraph accepts a UPCGGraph subclass, but this action creates an
			// exact UPCGGraph. Returning a subclass let rerunnable automation
			// proceed with subclass-specific behaviour, and later exact-UPCGGraph
			// operations such as complete replacement then reject the same asset.
			if (ExistingGraph->GetClass() != UPCGGraph::StaticClass())
			{
				return MonolithPCGAuthoring::InvalidParam(
					TEXT("asset_path"),
					FString::Printf(
						TEXT("Existing asset '%s' is a %s, not an exact UPCGGraph; existing_policy=return_existing cannot return it"),
						*ExistingObjectPath,
						*ExistingGraph->GetClass()->GetName()));
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

	// AssetExists and the disk-only DoesPackageExist both pass for an in-memory
	// unsaved package that holds no asset at the requested object path.
	// CreatePackage would then reuse it, so a successful create could save
	// unrelated objects, and DiscardCreatedGraph on a later failure clears the
	// shared package's dirty flag and hides those unsaved changes.
	if (FindPackage(nullptr, *PackageName) != nullptr)
	{
		return MonolithPCGAuthoring::InvalidParam(
			TEXT("asset_path"),
			FString::Printf(
				TEXT("An unsaved in-memory package already exists at '%s'; save or discard it before creating a graph there"),
				*PackageName));
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
	const TSharedPtr<FJsonValue> ExistingPolicyField = Params.IsValid()
		? Params->TryGetField(TEXT("existing_policy"))
		: nullptr;
	if (ExistingPolicyField.IsValid() && ExistingPolicyField->Type != EJson::Null &&
		(ExistingPolicyField->Type != EJson::String || !ExistingPolicyField->TryGetString(ExistingPolicy)))
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
	const TSharedPtr<FJsonValue> NodeTitleField = Params.IsValid()
		? Params->TryGetField(TEXT("node_title"))
		: nullptr;
	if (NodeTitleField.IsValid() && NodeTitleField->Type != EJson::Null &&
		(NodeTitleField->Type != EJson::String || !NodeTitleField->TryGetString(NodeTitle)))
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
		// Graph coordinates are int32. Narrowing an out-of-range or non-finite
		// double is undefined behaviour and previously placed the node at an
		// unrelated coordinate while the action still reported success.
		auto IsRepresentableCoordinate = [](double Value)
		{
			return FMath::IsFinite(Value)
				&& Value >= static_cast<double>(TNumericLimits<int32>::Min())
				&& Value <= static_cast<double>(TNumericLimits<int32>::Max());
		};
		if (!IsRepresentableCoordinate(X) || !IsRepresentableCoordinate(Y))
		{
			return MonolithPCGAuthoring::InvalidParam(
				TEXT("position"),
				TEXT("position values must be finite and within the int32 graph coordinate range"));
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
			MonolithPCGAuthoring::FinalizeGraphRollbackDirtyState(Graph, bWasDirty, bRolledBack);
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
				MonolithPCGAuthoring::FinalizeGraphRollbackDirtyState(Graph, bWasDirty, bRolledBack);
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
				MonolithPCGAuthoring::FinalizeGraphRollbackDirtyState(Graph, bWasDirty, bRolledBack);
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
				MonolithPCGAuthoring::FinalizeGraphRollbackDirtyState(Graph, bWasDirty, bRolledBack);
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
		MonolithPCGAuthoring::FinalizeGraphRollbackDirtyState(Graph, bWasDirty, bRolledBack);
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
		MonolithPCGAuthoring::FinalizeGraphRollbackDirtyState(Graph, bWasDirty, bRolledBack);
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
		const bool bRestored = MonolithPCGAuthoring::RestoreRemovedNode(
			Graph, Node, RemovedNodeName, IncidentEdges);
		MonolithPCGAuthoring::FinalizeGraphRollbackDirtyState(Graph, bWasDirty, bRestored);
		return FMonolithActionResult::Error(TEXT("UPCGGraph::RemoveNode postcondition failed"));
	}
	TSharedPtr<FJsonObject> ValidationReport;
	if (!MonolithPCGAuthoring::ValidateGraphForCommit(Graph, ObjectPath, ValidationReport, Error))
	{
		const bool bRestored = MonolithPCGAuthoring::RestoreRemovedNode(
			Graph, Node, RemovedNodeName, IncidentEdges);
		MonolithPCGAuthoring::FinalizeGraphRollbackDirtyState(Graph, bWasDirty, bRestored);
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
		MonolithPCGAuthoring::FinalizeGraphRollbackDirtyState(Graph, bWasDirty, bRestored);
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
			const bool bRolledBack = MonolithPCGAuthoring::RollbackConnectedEdge(
				Graph, SourceNode, FName(*SourcePinLabel), TargetNode, FName(*TargetPinLabel));
			MonolithPCGAuthoring::FinalizeGraphRollbackDirtyState(Graph, bWasDirty, bRolledBack);
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
			const bool bRolledBack = MonolithPCGAuthoring::RollbackConnectedEdge(
				Graph, SourceNode, FName(*SourcePinLabel), TargetNode, FName(*TargetPinLabel));
			MonolithPCGAuthoring::FinalizeGraphRollbackDirtyState(Graph, bWasDirty, bRolledBack);
			if (!bRolledBack)
			{
				Error += TEXT("; explicit edge rollback was incomplete");
			}
		}
		return FMonolithActionResult::Error(Error).WithErrorData(ValidationReport);
	}

	bool bSaved = false;
	FString SavedFilename;
	if (!MonolithPCGAuthoring::SaveGraph(Graph, bSave && !bAlreadyConnected, bSaved, SavedFilename, Error))
	{
		if (!bAlreadyConnected)
		{
			const bool bRolledBack = MonolithPCGAuthoring::RollbackConnectedEdge(
				Graph, SourceNode, FName(*SourcePinLabel), TargetNode, FName(*TargetPinLabel));
			MonolithPCGAuthoring::FinalizeGraphRollbackDirtyState(Graph, bWasDirty, bRolledBack);
			if (!bRolledBack)
			{
				Error += TEXT("; explicit edge rollback was incomplete");
			}
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
			const bool bRestored = MonolithPCGAuthoring::RollbackDisconnectedEdge(
				Graph, SourceNode, FName(*SourcePinLabel), TargetNode, FName(*TargetPinLabel));
			MonolithPCGAuthoring::FinalizeGraphRollbackDirtyState(Graph, bWasDirty, bRestored);
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
			MonolithPCGAuthoring::FinalizeGraphRollbackDirtyState(Graph, bWasDirty, bRestored);
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
			MonolithPCGAuthoring::FinalizeGraphRollbackDirtyState(Graph, bWasDirty, bRestored);
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

FMonolithActionResult FMonolithPCGGraphAuthoringActions::SetGraphUserParameters(
	const TSharedPtr<FJsonObject>& Params)
{
	FString AssetPath;
	FString Error;
	if (!MonolithPCGAuthoring::ReadRequiredString(Params, TEXT("asset_path"), AssetPath, Error))
	{
		return MonolithPCGAuthoring::InvalidParam(TEXT("asset_path"), Error);
	}
	bool bDryRun = true;
	bool bSave = true;
	if (!MonolithPCGAuthoring::ReadOptionalBool(Params, TEXT("dry_run"), true, bDryRun, Error) ||
		!MonolithPCGAuthoring::ReadOptionalBool(Params, TEXT("save"), true, bSave, Error))
	{
		return MonolithPCGAuthoring::InvalidParam(TEXT("params"), Error);
	}

	const TArray<TSharedPtr<FJsonValue>>* UpsertArray = nullptr;
	const TArray<TSharedPtr<FJsonValue>>* RemoveArray = nullptr;
	if (Params->HasField(TEXT("upsert")) && !Params->TryGetArrayField(TEXT("upsert"), UpsertArray))
	{
		return MonolithPCGAuthoring::InvalidParam(TEXT("upsert"), TEXT("upsert must be an array"));
	}
	if (Params->HasField(TEXT("remove")) && !Params->TryGetArrayField(TEXT("remove"), RemoveArray))
	{
		return MonolithPCGAuthoring::InvalidParam(TEXT("remove"), TEXT("remove must be an array of names"));
	}
	const int32 UpsertCount = UpsertArray ? UpsertArray->Num() : 0;
	const int32 RemoveCount = RemoveArray ? RemoveArray->Num() : 0;
	if (UpsertCount + RemoveCount == 0 ||
		UpsertCount + RemoveCount > MonolithPCGAuthoring::MaxGraphUserParameterOperations)
	{
		return MonolithPCGAuthoring::InvalidParam(TEXT("params"), FString::Printf(
			TEXT("Provide 1..%d total upsert/remove operations"),
			MonolithPCGAuthoring::MaxGraphUserParameterOperations));
	}

	UPCGGraph* Graph = nullptr;
	FString ObjectPath;
	if (!MonolithPCGAuthoring::LoadGraph(AssetPath, Graph, ObjectPath, Error))
	{
		return MonolithPCGAuthoring::InvalidParam(TEXT("asset_path"), Error);
	}
	const FInstancedPropertyBag* LiveBag = Graph->GetUserParametersStruct();
	if (!LiveBag)
	{
		return FMonolithActionResult::Error(TEXT("PCG graph has no user-parameter property bag"));
	}

	struct FUpsertOperation
	{
		FName Name;
		EPropertyBagPropertyType Type = EPropertyBagPropertyType::None;
		FString SerializedValue;
	};
	TArray<FUpsertOperation> Upserts;
	TArray<FName> Removes;
	TSet<FName> SeenNames;
	if (UpsertArray)
	{
		for (const TSharedPtr<FJsonValue>& Value : *UpsertArray)
		{
			const TSharedPtr<FJsonObject>* ObjectPtr = nullptr;
			if (!Value.IsValid() || !Value->TryGetObject(ObjectPtr) || !ObjectPtr || !ObjectPtr->IsValid())
			{
				return MonolithPCGAuthoring::InvalidParam(TEXT("upsert"),
					TEXT("Every upsert entry must be an object"));
			}
			FString NameString;
			FString TypeString;
			if (!(*ObjectPtr)->TryGetStringField(TEXT("name"), NameString) ||
				!(*ObjectPtr)->TryGetStringField(TEXT("type"), TypeString) ||
				!(*ObjectPtr)->HasField(TEXT("default_value")))
			{
				return MonolithPCGAuthoring::InvalidParam(TEXT("upsert"),
					TEXT("Every upsert entry requires string name, string type, and default_value"));
			}
			NameString.TrimStartAndEndInline();
			TypeString.TrimStartAndEndInline();
			const FName Name(*NameString);
			if (!FInstancedPropertyBag::IsPropertyNameValid(Name))
			{
				return MonolithPCGAuthoring::InvalidParam(TEXT("upsert"),
					FString::Printf(TEXT("Invalid user-parameter name '%s'"), *NameString));
			}
			if (SeenNames.Contains(Name))
			{
				return MonolithPCGAuthoring::InvalidParam(TEXT("upsert"), FString::Printf(
					TEXT("Duplicate case-insensitive user-parameter name '%s'"), *NameString));
			}
			FUpsertOperation Operation;
			Operation.Name = Name;
			if (!MonolithPCGAuthoring::ParseGraphUserParameterType(TypeString, Operation.Type))
			{
				return MonolithPCGAuthoring::InvalidParam(TEXT("upsert"), FString::Printf(
					TEXT("Unsupported type '%s'; use bool, byte, int32, int64, float, double, name, or string"),
					*TypeString));
			}
			if (!MonolithPCGAuthoring::GraphUserParameterJsonToSerialized(Operation.Type,
				(*ObjectPtr)->TryGetField(TEXT("default_value")), Operation.SerializedValue, Error))
			{
				return MonolithPCGAuthoring::InvalidParam(TEXT("upsert"), FString::Printf(
					TEXT("Invalid default_value for '%s': %s"), *NameString, *Error));
			}
			SeenNames.Add(Name);
			Upserts.Add(MoveTemp(Operation));
		}
	}
	if (RemoveArray)
	{
		for (const TSharedPtr<FJsonValue>& Value : *RemoveArray)
		{
			FString NameString;
			if (!Value.IsValid() || Value->Type != EJson::String || !Value->TryGetString(NameString))
			{
				return MonolithPCGAuthoring::InvalidParam(TEXT("remove"),
					TEXT("Every remove entry must be a string"));
			}
			NameString.TrimStartAndEndInline();
			const FName Name(*NameString);
			if (NameString.IsEmpty() || Name.IsNone() || SeenNames.Contains(Name))
			{
				return MonolithPCGAuthoring::InvalidParam(TEXT("remove"), FString::Printf(
					TEXT("Invalid, duplicate, or conflicting remove name '%s'"), *NameString));
			}
			if (!LiveBag->FindPropertyDescByName(Name))
			{
				return MonolithPCGAuthoring::InvalidParam(TEXT("remove"), FString::Printf(
					TEXT("Unknown graph user parameter '%s'"), *NameString));
			}
			SeenNames.Add(Name);
			Removes.Add(Name);
		}
	}

	FInstancedPropertyBag StagedBag = *LiveBag;
	if (!Removes.IsEmpty() &&
		StagedBag.RemovePropertiesByName(Removes) != EPropertyBagAlterationResult::Success)
	{
		return FMonolithActionResult::Error(TEXT("UE property bag rejected the staged parameter removals"));
	}
	for (const FUpsertOperation& Operation : Upserts)
	{
		const FPropertyBagPropertyDesc* Existing = StagedBag.FindPropertyDescByName(Operation.Name);
		if (!Existing || Existing->ValueType != Operation.Type || !Existing->ContainerTypes.IsEmpty())
		{
			const FPropertyBagPropertyDesc Descriptor(Operation.Name, Operation.Type);
			if (StagedBag.AddProperties({Descriptor}, true) != EPropertyBagAlterationResult::Success)
			{
				return FMonolithActionResult::Error(FString::Printf(
					TEXT("UE property bag rejected staged schema for '%s'"), *Operation.Name.ToString()));
			}
		}
		if (StagedBag.SetValueSerializedString(Operation.Name, Operation.SerializedValue) !=
			EPropertyBagResult::Success)
		{
			return MonolithPCGAuthoring::InvalidParam(TEXT("upsert"), FString::Printf(
				TEXT("UE property bag rejected default_value for '%s'"), *Operation.Name.ToString()));
		}
	}

	auto BuildRows = [&]()
	{
		TArray<TSharedPtr<FJsonValue>> Rows;
		for (const FUpsertOperation& Operation : Upserts)
		{
			const FPropertyBagPropertyDesc* BeforeDesc = LiveBag->FindPropertyDescByName(Operation.Name);
			const FPropertyBagPropertyDesc* AfterDesc = StagedBag.FindPropertyDescByName(Operation.Name);
			TSharedPtr<FJsonObject> Row = MakeShared<FJsonObject>();
			Row->SetStringField(TEXT("name"), Operation.Name.ToString());
			Row->SetStringField(TEXT("operation"), BeforeDesc ? TEXT("update") : TEXT("add"));
			Row->SetStringField(TEXT("before_type"), BeforeDesc
				? MonolithPCGAuthoring::GraphUserParameterTypeToString(BeforeDesc->ValueType) : TEXT("missing"));
			Row->SetStringField(TEXT("after_type"), AfterDesc
				? MonolithPCGAuthoring::GraphUserParameterTypeToString(AfterDesc->ValueType) : TEXT("missing"));
			const TValueOrError<FString, EPropertyBagResult> AfterValue =
				StagedBag.GetValueSerializedString(Operation.Name);
			Row->SetStringField(TEXT("after_serialized_value"),
				AfterValue.IsValid() ? AfterValue.GetValue() : FString());
			Rows.Add(MakeShared<FJsonValueObject>(Row));
		}
		for (const FName Name : Removes)
		{
			const FPropertyBagPropertyDesc* BeforeDesc = LiveBag->FindPropertyDescByName(Name);
			TSharedPtr<FJsonObject> Row = MakeShared<FJsonObject>();
			Row->SetStringField(TEXT("name"), Name.ToString());
			Row->SetStringField(TEXT("operation"), TEXT("remove"));
			Row->SetStringField(TEXT("before_type"), BeforeDesc
				? MonolithPCGAuthoring::GraphUserParameterTypeToString(BeforeDesc->ValueType) : TEXT("missing"));
			Row->SetStringField(TEXT("after_type"), TEXT("missing"));
			Rows.Add(MakeShared<FJsonValueObject>(Row));
		}
		return Rows;
	};

	bool bChanged = !Removes.IsEmpty();
	for (const FUpsertOperation& Operation : Upserts)
	{
		const FPropertyBagPropertyDesc* BeforeDesc = LiveBag->FindPropertyDescByName(Operation.Name);
		const FPropertyBagPropertyDesc* AfterDesc = StagedBag.FindPropertyDescByName(Operation.Name);
		if (!BeforeDesc || !AfterDesc || BeforeDesc->ValueType != AfterDesc->ValueType ||
			BeforeDesc->ContainerTypes != AfterDesc->ContainerTypes)
		{
			bChanged = true;
			continue;
		}
		const TValueOrError<FString, EPropertyBagResult> BeforeValue =
			LiveBag->GetValueSerializedString(Operation.Name);
		const TValueOrError<FString, EPropertyBagResult> AfterValue =
			StagedBag.GetValueSerializedString(Operation.Name);
		if (!BeforeValue.IsValid() || !AfterValue.IsValid() || BeforeValue.GetValue() != AfterValue.GetValue())
		{
			bChanged = true;
		}
	}
	const int32 ParameterCountBefore =
		LiveBag->GetPropertyBagStruct() ? LiveBag->GetPropertyBagStruct()->GetPropertyDescs().Num() : 0;
	const int32 ParameterCountAfter =
		StagedBag.GetPropertyBagStruct() ? StagedBag.GetPropertyBagStruct()->GetPropertyDescs().Num() : 0;
	const TArray<TSharedPtr<FJsonValue>> ChangeRows = BuildRows();
	auto BuildResult = [&](const TCHAR* Status, bool bSaved, const FString& SavedFilename)
	{
		TSharedPtr<FJsonObject> Result = MonolithPCGAuthoring::MutationResult(
			TEXT("set_pcg_graph_user_parameters"), ObjectPath, Status, bSaved, SavedFilename);
		Result->SetBoolField(TEXT("dry_run"), bDryRun);
		Result->SetBoolField(TEXT("changed"), bChanged);
		Result->SetNumberField(TEXT("operation_count"), UpsertCount + RemoveCount);
		Result->SetNumberField(TEXT("parameter_count_before"), ParameterCountBefore);
		Result->SetNumberField(TEXT("parameter_count_after"), ParameterCountAfter);
		Result->SetArrayField(TEXT("changes"), ChangeRows);
		return Result;
	};
	if (bDryRun)
	{
		return FMonolithActionResult::Success(BuildResult(
			bChanged ? TEXT("would_update") : TEXT("unchanged"), false, FString()));
	}
	if (!bChanged)
	{
		return FMonolithActionResult::Success(BuildResult(TEXT("unchanged"), false, FString()));
	}

	const FInstancedPropertyBag OriginalBag = *LiveBag;
	const bool bWasDirty = Graph->GetPackage()->IsDirty();
	Graph->Modify();
	Graph->UpdateUserParametersStruct([&StagedBag](FInstancedPropertyBag& Parameters)
	{
		Parameters = StagedBag;
	});
	Graph->MarkPackageDirty();
	TSharedPtr<FJsonObject> ValidationReport;
	if (!MonolithPCGAuthoring::ValidateGraphForCommit(Graph, ObjectPath, ValidationReport, Error))
	{
		const bool bRestored =
			MonolithPCGAuthoring::RestoreGraphUserParameters(Graph, OriginalBag, bWasDirty);
		if (!bRestored)
		{
			Error += TEXT("; graph user-parameter rollback was incomplete");
		}
		return FMonolithActionResult::Error(Error).WithErrorData(ValidationReport);
	}
	bool bSaved = false;
	FString SavedFilename;
	if (!MonolithPCGAuthoring::SaveGraph(Graph, bSave, bSaved, SavedFilename, Error))
	{
		const bool bRestored =
			MonolithPCGAuthoring::RestoreGraphUserParameters(Graph, OriginalBag, bWasDirty);
		if (!bRestored)
		{
			Error += TEXT("; graph user-parameter rollback was incomplete");
		}
		return FMonolithActionResult::Error(Error);
	}
	return FMonolithActionResult::Success(BuildResult(TEXT("updated"), bSaved, SavedFilename));
}

FMonolithActionResult FMonolithPCGGraphAuthoringActions::SetSubgraph(const TSharedPtr<FJsonObject>& Params)
{
	FString AssetPath;
	FString NodeIdentifier;
	FString SubgraphAssetPath;
	FString Error;
	if (!MonolithPCGAuthoring::ReadRequiredString(Params, TEXT("asset_path"), AssetPath, Error) ||
		!MonolithPCGAuthoring::ReadRequiredString(Params, TEXT("node_id"), NodeIdentifier, Error) ||
		!MonolithPCGAuthoring::ReadRequiredString(
			Params, TEXT("subgraph_asset_path"), SubgraphAssetPath, Error))
	{
		return MonolithPCGAuthoring::InvalidParam(TEXT("params"), Error);
	}

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
	UPCGSubgraphSettings* Settings = Cast<UPCGSubgraphSettings>(Node->GetSettings());
	if (!Settings || !Settings->SubgraphInstance)
	{
		return MonolithPCGAuthoring::InvalidParam(
			TEXT("node_id"),
			FString::Printf(TEXT("PCG node '%s' must use UPCGSubgraphSettings, got %s"),
				*NodeIdentifier,
				Node->GetSettings() ? *Node->GetSettings()->GetClass()->GetName() : TEXT("no settings")));
	}
	if (Settings->SubgraphOverride)
	{
		return MonolithPCGAuthoring::InvalidParam(
			TEXT("node_id"),
			TEXT("The subgraph node has an active SubgraphOverride; refusing to change a hidden default "
				 "assignment that would not be the effective graph"));
	}

	UPCGGraphInterface* RequestedInterface = nullptr;
	FString RequestedObjectPath;
	if (!MonolithPCGAuthoring::LoadGraphInterface(
			SubgraphAssetPath, RequestedInterface, RequestedObjectPath, Error))
	{
		return MonolithPCGAuthoring::InvalidParam(TEXT("subgraph_asset_path"), Error);
	}
	if (!Settings->SubgraphInstance->CanGraphInterfaceBeSet(RequestedInterface))
	{
		return MonolithPCGAuthoring::InvalidParam(
			TEXT("subgraph_asset_path"),
			FString::Printf(
				TEXT("Assigning '%s' would create a recursive PCG graph-instance hierarchy"),
				*RequestedObjectPath));
	}
	UPCGGraph* RequestedGraph = RequestedInterface->GetGraph();
	if (RequestedGraph == Graph || RequestedGraph->Contains(Graph))
	{
		return MonolithPCGAuthoring::InvalidParam(
			TEXT("subgraph_asset_path"),
			FString::Printf(TEXT("Assigning '%s' to '%s' would create a recursive PCG subgraph hierarchy"),
				*RequestedObjectPath, *ObjectPath));
	}

	IAssetRegistry& AssetRegistry =
		FModuleManager::LoadModuleChecked<FAssetRegistryModule>(TEXT("AssetRegistry")).Get();
	const FAssetData RequestedAssetData =
		AssetRegistry.GetAssetByObjectPath(FSoftObjectPath(RequestedObjectPath));
	if (Graph->GraphCustomization.FiltersSubgraphs() && !RequestedAssetData.IsValid())
	{
		return MonolithPCGAuthoring::InvalidParam(
			TEXT("subgraph_asset_path"),
			FString::Printf(
				TEXT("AssetRegistry metadata is required to evaluate the parent graph's subgraph filter: '%s'"),
				*RequestedObjectPath));
	}
	if (RequestedAssetData.IsValid() && Settings->SubgraphAssetFilter(RequestedAssetData))
	{
		return MonolithPCGAuthoring::InvalidParam(
			TEXT("subgraph_asset_path"),
			FString::Printf(TEXT("Parent graph customization rejects subgraph '%s'"), *RequestedObjectPath));
	}

	UPCGGraphInterface* PreviousInterface = Settings->SubgraphInstance->Graph.Get();
	const FString PreviousPath = PreviousInterface ? PreviousInterface->GetPathName() : FString();
	const bool bWouldChange = PreviousInterface != RequestedInterface;
	auto BuildResult = [&](const TCHAR* Status, bool bSaved, const FString& SavedFilename)
	{
		TSharedPtr<FJsonObject> Result = MonolithPCGAuthoring::MutationResult(
			TEXT("set_pcg_subgraph"), ObjectPath, Status, bSaved, SavedFilename);
		Result->SetStringField(TEXT("node_id"), MonolithPCGAuthoring::NodeId(Graph, Node));
		Result->SetStringField(TEXT("node_path"), Node->GetPathName());
		Result->SetStringField(TEXT("settings_class"), Settings->GetClass()->GetName());
		Result->SetStringField(TEXT("previous_subgraph_path"), PreviousPath);
		Result->SetStringField(TEXT("requested_subgraph_path"), RequestedObjectPath);
		Result->SetStringField(TEXT("requested_graph_path"), RequestedGraph->GetPathName());
		UPCGGraphInterface* AssignedInterface =
			Settings->SubgraphInstance ? Settings->SubgraphInstance->Graph.Get() : nullptr;
		UPCGGraph* AssignedGraph = Settings->GetSubgraph();
		Result->SetStringField(
			TEXT("assigned_subgraph_path"), AssignedInterface ? AssignedInterface->GetPathName() : FString());
		Result->SetStringField(
			TEXT("assigned_graph_path"), AssignedGraph ? AssignedGraph->GetPathName() : FString());
		Result->SetNumberField(
			TEXT("incident_edge_count"), MonolithPCGAuthoring::CaptureIncidentEdges(Graph, Node).Num());
		Result->SetBoolField(TEXT("changed"), bWouldChange);
		Result->SetBoolField(TEXT("dry_run"), bDryRun);
		MonolithPCGAuthoring::AddBoundedPinFields(Result, Node);
		return Result;
	};

	if (bDryRun)
	{
		return FMonolithActionResult::Success(
			BuildResult(bWouldChange ? TEXT("would_update") : TEXT("unchanged"), false, FString()));
	}
	if (!bWouldChange)
	{
		return FMonolithActionResult::Success(BuildResult(TEXT("unchanged"), false, FString()));
	}

	const bool bWasDirty = Graph->GetPackage()->IsDirty();
	const TArray<MonolithPCGAuthoring::FGraphEdgeDescriptor> OriginalIncidentEdges =
		MonolithPCGAuthoring::CaptureIncidentEdges(Graph, Node);
	{
		FMonolithPCGScopedGraphEditNotifications NotificationScope(Graph);
		Graph->Modify();
		Node->Modify();
		Settings->Modify();
		Settings->SetSubgraph(RequestedInterface);
		NotificationScope.MarkExternalModification();
	}

	const bool bReadBackMatches = Settings->SubgraphInstance &&
		Settings->SubgraphInstance->Graph.Get() == RequestedInterface &&
		Settings->GetSubgraph() == RequestedGraph;
	if (!bReadBackMatches)
	{
		const bool bRestored = MonolithPCGAuthoring::RestoreSubgraphMutation(
			Graph, Node, Settings, PreviousInterface, OriginalIncidentEdges, bWasDirty);
		return FMonolithActionResult::Error(
			bRestored ? TEXT("UPCGBaseSubgraphSettings::SetSubgraph did not preserve the exact requested interface")
					  : TEXT("Subgraph assignment read-back failed and rollback was incomplete"));
	}
	if (!MonolithPCGAuthoring::AreEdgeDescriptorsEqual(
			MonolithPCGAuthoring::CaptureIncidentEdges(Graph, Node), OriginalIncidentEdges))
	{
		const bool bRestored = MonolithPCGAuthoring::RestoreSubgraphMutation(
			Graph, Node, Settings, PreviousInterface, OriginalIncidentEdges, bWasDirty);
		return FMonolithActionResult::Error(
			bRestored
				? TEXT("Subgraph assignment changed incident topology; disconnect or reconnect edges explicitly")
				: TEXT("Subgraph assignment changed incident topology and rollback was incomplete"));
	}

	Graph->MarkPackageDirty();
	TSharedPtr<FJsonObject> ValidationReport;
	if (!MonolithPCGAuthoring::ValidateGraphForCommit(Graph, ObjectPath, ValidationReport, Error))
	{
		const bool bRestored = MonolithPCGAuthoring::RestoreSubgraphMutation(
			Graph, Node, Settings, PreviousInterface, OriginalIncidentEdges, bWasDirty);
		if (!bRestored)
		{
			Error += TEXT("; subgraph assignment rollback was incomplete");
		}
		return FMonolithActionResult::Error(Error).WithErrorData(ValidationReport);
	}

	bool bSaved = false;
	FString SavedFilename;
	if (!MonolithPCGAuthoring::SaveGraph(Graph, bSave, bSaved, SavedFilename, Error))
	{
		const bool bRestored = MonolithPCGAuthoring::RestoreSubgraphMutation(
			Graph, Node, Settings, PreviousInterface, OriginalIncidentEdges, bWasDirty);
		if (!bRestored)
		{
			Error += TEXT("; subgraph assignment rollback was incomplete");
		}
		return FMonolithActionResult::Error(Error);
	}

	return FMonolithActionResult::Success(BuildResult(TEXT("updated"), bSaved, SavedFilename));
}

FMonolithActionResult FMonolithPCGGraphAuthoringActions::ReplaceGraphContents(
	const TSharedPtr<FJsonObject>& Params)
{
	FString SourceAssetPath;
	FString TargetAssetPath;
	FString Error;
	if (!MonolithPCGAuthoring::ReadRequiredString(
			Params, TEXT("source_asset_path"), SourceAssetPath, Error) ||
		!MonolithPCGAuthoring::ReadRequiredString(
			Params, TEXT("target_asset_path"), TargetAssetPath, Error))
	{
		return MonolithPCGAuthoring::InvalidParam(TEXT("params"), Error);
	}

	bool bDryRun = true;
	bool bConfirm = false;
	bool bSave = true;
	int32 NodeLimit = MonolithPCGAuthoring::MaxGraphNodes;
	int32 EdgeLimit = MonolithPCGAuthoring::MaxGraphEdges;
	if (!MonolithPCGAuthoring::ReadOptionalBool(Params, TEXT("dry_run"), true, bDryRun, Error) ||
		!MonolithPCGAuthoring::ReadOptionalBool(Params, TEXT("confirm"), false, bConfirm, Error) ||
		!MonolithPCGAuthoring::ReadOptionalBool(Params, TEXT("save"), true, bSave, Error) ||
		!MonolithPCGAuthoring::ReadOptionalInt(
			Params, TEXT("node_limit"), MonolithPCGAuthoring::MaxGraphNodes, 1,
			MonolithPCGAuthoring::MaxGraphNodes, NodeLimit, Error) ||
		!MonolithPCGAuthoring::ReadOptionalInt(
			Params, TEXT("edge_limit"), MonolithPCGAuthoring::MaxGraphEdges, 1,
			MonolithPCGAuthoring::MaxGraphEdges, EdgeLimit, Error))
	{
		return MonolithPCGAuthoring::InvalidParam(TEXT("params"), Error);
	}
	if (!bDryRun && !bConfirm)
	{
		return MonolithPCGAuthoring::InvalidParam(
			TEXT("confirm"),
			TEXT("confirm=true is required when dry_run=false because the target graph contents are replaced"));
	}

	UPCGGraph* Source = nullptr;
	UPCGGraph* Target = nullptr;
	FString SourceObjectPath;
	FString TargetObjectPath;
	if (!MonolithPCGAuthoring::LoadGraph(SourceAssetPath, Source, SourceObjectPath, Error))
	{
		return MonolithPCGAuthoring::InvalidParam(TEXT("source_asset_path"), Error);
	}
	if (!MonolithPCGAuthoring::LoadGraph(TargetAssetPath, Target, TargetObjectPath, Error))
	{
		return MonolithPCGAuthoring::InvalidParam(TEXT("target_asset_path"), Error);
	}
	if (Source == Target || SourceObjectPath.Equals(TargetObjectPath, ESearchCase::CaseSensitive))
	{
		return MonolithPCGAuthoring::InvalidParam(
			TEXT("target_asset_path"), TEXT("source_asset_path and target_asset_path must identify distinct graphs"));
	}
	if (Source->GetClass() != UPCGGraph::StaticClass() || Target->GetClass() != UPCGGraph::StaticClass())
	{
		return MonolithPCGAuthoring::InvalidParam(
			TEXT("params"),
			FString::Printf(
				TEXT("source and target must both be exact UPCGGraph assets (source=%s, target=%s)"),
				*Source->GetClass()->GetPathName(), *Target->GetClass()->GetPathName()));
	}
	if (Source->Contains(Source))
	{
		return MonolithPCGAuthoring::InvalidParam(
			TEXT("source_asset_path"),
			TEXT("source graph contains a recursive static-subgraph hierarchy"));
	}
	if (Source->Contains(Target))
	{
		return MonolithPCGAuthoring::InvalidParam(
			TEXT("source_asset_path"),
			TEXT("source graph contains the target graph; replacement would create a recursive target hierarchy"));
	}
	if (Source->GetPackage()->IsDirty())
	{
		return MonolithPCGAuthoring::InvalidParam(
			TEXT("source_asset_path"),
			TEXT("source graph package has unsaved changes; save or revert it before using it as an exact replacement source"));
	}
	if (Target->GetPackage()->IsDirty())
	{
		return MonolithPCGAuthoring::InvalidParam(
			TEXT("target_asset_path"),
			TEXT("target graph package has unsaved changes; save or revert it before replacing its complete contents"));
	}

	const int32 SourceNodeCount = Source->GetNodes().Num();
	const int32 TargetNodeCountBefore = Target->GetNodes().Num();
	const int32 SourceEdgeCount = MonolithPCGAuthoring::GetGraphEdges(Source).Num();
	const int32 TargetEdgeCountBefore = MonolithPCGAuthoring::GetGraphEdges(Target).Num();
	if (SourceNodeCount > NodeLimit || TargetNodeCountBefore > NodeLimit)
	{
		return MonolithPCGAuthoring::InvalidParam(
			TEXT("node_limit"),
			FString::Printf(
				TEXT("source/target element node counts (%d/%d) exceed node_limit=%d"),
				SourceNodeCount, TargetNodeCountBefore, NodeLimit));
	}
	if (SourceEdgeCount > EdgeLimit || TargetEdgeCountBefore > EdgeLimit)
	{
		return MonolithPCGAuthoring::InvalidParam(
			TEXT("edge_limit"),
			FString::Printf(
				TEXT("source/target edge counts (%d/%d) exceed edge_limit=%d"),
				SourceEdgeCount, TargetEdgeCountBefore, EdgeLimit));
	}

	TSharedPtr<FJsonObject> SourceValidation;
	if (!MonolithPCGAuthoring::ValidateGraphForCommit(
			Source, SourceObjectPath, SourceValidation, Error))
	{
		return FMonolithActionResult::Error(
			FString::Printf(TEXT("source graph is not structurally safe to clone: %s"), *Error))
			.WithErrorData(SourceValidation);
	}

	UPCGGraph* Preview = MonolithPCGAuthoring::DuplicateGraphToTransient(
		Target, TEXT("MonolithPCGGraphReplacementPreview"), Error);
	if (!Preview)
	{
		return FMonolithActionResult::Error(Error);
	}
	ON_SCOPE_EXIT
	{
		MonolithPCGAuthoring::DiscardTransientGraph(Preview);
	};
	if (!MonolithPCGAuthoring::ReplaceGraphObjectState(Source, Preview, false, Error))
	{
		return FMonolithActionResult::Error(
			FString::Printf(TEXT("could not build an exact transient replacement preview: %s"), *Error));
	}
	TSharedPtr<FJsonObject> PreviewValidation;
	if (!MonolithPCGAuthoring::ValidateGraphForCommit(
			Preview, Preview->GetPathName(), PreviewValidation, Error))
	{
		return FMonolithActionResult::Error(
			FString::Printf(TEXT("transient replacement preview is structurally invalid: %s"), *Error))
			.WithErrorData(PreviewValidation);
	}

	FString ExistingMismatch;
	FString ExistingComparisonError;
	const bool bExistingPropertiesIdentical =
		MonolithPCGAuthoring::ArePersistentGraphPropertiesIdentical(
			Target, Preview, ExistingMismatch, ExistingComparisonError);
	if (!ExistingComparisonError.IsEmpty())
	{
		return FMonolithActionResult::Error(FString::Printf(
			TEXT("could not compare target and replacement preview within persistent-state bounds: %s"),
			*ExistingComparisonError));
	}
	const bool bWouldChange = !bExistingPropertiesIdentical;
	const FString TargetIdentityBefore = Target->GetPathName();
	const int32 TargetUserParameterCountBefore = MonolithPCGAuthoring::GetUserParameterCount(Target);
	auto BuildResult = [&](const TCHAR* Status, bool bSaved, const FString& SavedFilename,
		const TCHAR* RollbackStatus)
	{
		TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
		Result->SetStringField(TEXT("namespace"), TEXT("pcg"));
		Result->SetStringField(TEXT("action"), TEXT("replace_pcg_graph_contents"));
		Result->SetStringField(TEXT("status"), Status);
		Result->SetStringField(TEXT("source_asset_path"), SourceObjectPath);
		Result->SetStringField(TEXT("target_asset_path"), TargetObjectPath);
		Result->SetStringField(TEXT("target_identity_before"), TargetIdentityBefore);
		Result->SetStringField(TEXT("target_identity_after"), Target->GetPathName());
		Result->SetStringField(TEXT("source_class"), Source->GetClass()->GetPathName());
		Result->SetStringField(TEXT("target_class"), Target->GetClass()->GetPathName());
		Result->SetBoolField(TEXT("dry_run"), bDryRun);
		Result->SetBoolField(TEXT("confirmed"), bConfirm);
		Result->SetBoolField(TEXT("changed"), bWouldChange);
		Result->SetBoolField(TEXT("saved"), bSaved);
		Result->SetBoolField(TEXT("target_identity_preserved"),
			Target->GetPathName().Equals(TargetIdentityBefore, ESearchCase::CaseSensitive));
		Result->SetBoolField(TEXT("preview_persistent_properties_verified"), true);
		Result->SetBoolField(TEXT("persistent_properties_verified"), true);
		Result->SetStringField(TEXT("rollback_status"), RollbackStatus);
		Result->SetNumberField(TEXT("node_limit"), NodeLimit);
		Result->SetNumberField(TEXT("edge_limit"), EdgeLimit);
		Result->SetNumberField(TEXT("source_element_node_count"), SourceNodeCount);
		Result->SetNumberField(TEXT("source_edge_count"), SourceEdgeCount);
		Result->SetNumberField(TEXT("source_user_parameter_count"),
			MonolithPCGAuthoring::GetUserParameterCount(Source));
		Result->SetNumberField(TEXT("target_element_node_count_before"), TargetNodeCountBefore);
		Result->SetNumberField(TEXT("target_edge_count_before"), TargetEdgeCountBefore);
		Result->SetNumberField(TEXT("target_user_parameter_count_before"),
			TargetUserParameterCountBefore);
		Result->SetNumberField(TEXT("target_element_node_count_after"), Target->GetNodes().Num());
		Result->SetNumberField(TEXT("target_edge_count_after"),
			MonolithPCGAuthoring::GetGraphEdges(Target).Num());
		Result->SetNumberField(TEXT("target_user_parameter_count_after"),
			MonolithPCGAuthoring::GetUserParameterCount(Target));
		if (!ExistingMismatch.IsEmpty())
		{
			Result->SetStringField(TEXT("first_changed_persistent_property"), ExistingMismatch);
		}
		if (!SavedFilename.IsEmpty())
		{
			Result->SetStringField(TEXT("saved_filename"), SavedFilename);
		}
		if (bDryRun || !bWouldChange)
		{
			TSharedPtr<FJsonObject> Prepare = MakeShared<FJsonObject>();
			Prepare->SetStringField(TEXT("mode"), TEXT("handler_owned_pre_mutation"));
			Prepare->SetStringField(
				TEXT("status"), bDryRun ? TEXT("skipped_by_dry_run") : TEXT("skipped_no_change"));
			Result->SetObjectField(TEXT("source_control_prepare"), Prepare);
		}
		return Result;
	};

	if (bDryRun)
	{
		return FMonolithActionResult::Success(BuildResult(
			bWouldChange ? TEXT("would_replace") : TEXT("unchanged"),
			false, FString(), TEXT("not_required_dry_run")));
	}
	if (!bWouldChange)
	{
		return FMonolithActionResult::Success(
			BuildResult(TEXT("unchanged"), false, FString(), TEXT("not_required_no_change")));
	}

	TSharedPtr<FJsonObject> SourceControlPrepare;
	FMonolithActionResult SourceControlError;
	if (!MonolithPCGAuthoring::PrepareGraphReplacementSourceControl(
			Target, SourceControlPrepare, SourceControlError))
	{
		return SourceControlError;
	}

	UPCGGraph* OriginalSnapshot = MonolithPCGAuthoring::DuplicateGraphToTransient(
		Target, TEXT("MonolithPCGGraphReplacementRollback"), Error);
	if (!OriginalSnapshot)
	{
		return MonolithPCGAuthoring::AttachGraphReplacementSourceControl(
			FMonolithActionResult::Error(Error), SourceControlPrepare);
	}
	ON_SCOPE_EXIT
	{
		MonolithPCGAuthoring::DiscardTransientGraph(OriginalSnapshot);
	};
	const bool bWasDirty = Target->GetPackage()->IsDirty();
	auto RollbackFailure = [&](const FString& Failure) -> FMonolithActionResult
	{
		FString RollbackError;
		const bool bRolledBack = MonolithPCGAuthoring::RestoreGraphFromSnapshot(
			Target, OriginalSnapshot, bWasDirty, RollbackError);
		TSharedPtr<FJsonObject> Data = BuildResult(
			TEXT("failed"), false, FString(), bRolledBack ? TEXT("verified") : TEXT("incomplete"));
		Data->SetBoolField(TEXT("persistent_properties_verified"), false);
		Data->SetBoolField(TEXT("rollback_persistent_properties_verified"), bRolledBack);
		Data->SetNumberField(TEXT("target_user_parameter_count_before"), TargetUserParameterCountBefore);
		if (!RollbackError.IsEmpty())
		{
			Data->SetStringField(TEXT("rollback_error"), RollbackError);
		}
		return MonolithPCGAuthoring::AttachGraphReplacementSourceControl(FMonolithActionResult::Error(
			FString::Printf(
				TEXT("%s; rollback=%s%s%s"), *Failure,
				bRolledBack ? TEXT("verified") : TEXT("INCOMPLETE"),
				RollbackError.IsEmpty() ? TEXT("") : TEXT(": "), *RollbackError))
			.WithErrorData(Data), SourceControlPrepare);
	};

	if (!MonolithPCGAuthoring::ReplaceGraphObjectState(Source, Target, true, Error))
	{
		return RollbackFailure(FString::Printf(TEXT("target graph replacement failed: %s"), *Error));
	}
	if (!Target->GetPathName().Equals(TargetIdentityBefore, ESearchCase::CaseSensitive))
	{
		return RollbackFailure(TEXT("target graph identity changed during replacement"));
	}
	FString AppliedMismatch;
	FString AppliedComparisonError;
	if (!MonolithPCGAuthoring::ArePersistentGraphPropertiesIdentical(
			Preview, Target, AppliedMismatch, AppliedComparisonError))
	{
		return RollbackFailure(!AppliedComparisonError.IsEmpty()
			? FString::Printf(
				TEXT("target read-back comparison could not complete: %s"),
				*AppliedComparisonError)
			: FString::Printf(
				TEXT("target read-back differs from the verified preview at persistent property '%s'"),
				*AppliedMismatch));
	}
	Target->MarkPackageDirty();
	TSharedPtr<FJsonObject> TargetValidation;
	if (!MonolithPCGAuthoring::ValidateGraphForCommit(
			Target, TargetObjectPath, TargetValidation, Error))
	{
		FMonolithActionResult Failure = RollbackFailure(Error);
		MonolithPCGResultUtils::EnsureErrorDataObject(Failure)->SetObjectField(
			TEXT("target_validation"), TargetValidation);
		return Failure;
	}

#if WITH_DEV_AUTOMATION_TESTS
	if (UE::MonolithPCG::Private::ConsumeGraphContentsReplacementTestFault(
			TargetObjectPath,
			UE::MonolithPCG::Private::EPCGGraphContentsReplacementTestFault::BeforeSave))
	{
		return RollbackFailure(TEXT("injected graph replacement failure before save"));
	}
#endif

	bool bSaved = false;
	FString SavedFilename;
	if (!MonolithPCGAuthoring::SaveGraph(Target, bSave, bSaved, SavedFilename, Error))
	{
		return RollbackFailure(Error);
	}
	TSharedPtr<FJsonObject> Result = BuildResult(
		TEXT("replaced"), bSaved, SavedFilename, TEXT("not_required"));
	Result->SetNumberField(TEXT("target_user_parameter_count_before"), TargetUserParameterCountBefore);
	return MonolithPCGAuthoring::AttachGraphReplacementSourceControl(
		FMonolithActionResult::Success(Result), SourceControlPrepare);
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
