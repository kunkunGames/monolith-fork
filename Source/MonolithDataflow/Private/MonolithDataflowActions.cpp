#include "MonolithDataflowActions.h"

#include "MonolithParamSchema.h"

#include "AssetRegistry/AssetRegistryModule.h"
#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "Modules/ModuleManager.h"

#if WITH_MONOLITH_DATAFLOW
#include "Dataflow/DataflowEdNode.h"
#include "Dataflow/DataflowGraph.h"
#include "Dataflow/DataflowInputOutput.h"
#include "Dataflow/DataflowNode.h"
#include "Dataflow/DataflowNodeFactory.h"
#include "Dataflow/DataflowObject.h"
#include "EdGraphNode_Comment.h"
#include "Misc/PackageName.h"
#include "StructUtils/PropertyBag.h"
#include "UObject/UObjectGlobals.h"
#include "UObject/UnrealType.h"
#endif

namespace MonolithDataflow
{
	int32 ClampLimit(double LimitValue)
	{
		return FMath::Clamp(static_cast<int32>(LimitValue), 1, 500);
	}

	int32 GetClampedIntParam(const TSharedPtr<FJsonObject>& Params, const TCHAR* Name, int32 DefaultValue, int32 MinValue, int32 MaxValue)
	{
		double RawValue = static_cast<double>(DefaultValue);
		Params->TryGetNumberField(Name, RawValue);
		return FMath::Clamp(static_cast<int32>(RawValue), MinValue, MaxValue);
	}

	bool GetBoolParam(const TSharedPtr<FJsonObject>& Params, const TCHAR* Name, bool bDefaultValue)
	{
		bool bValue = bDefaultValue;
		Params->TryGetBoolField(Name, bValue);
		return bValue;
	}

	FMonolithActionResult ErrorWithCode(const FString& Code, const FString& Detail, const FString& AssetPath = FString())
	{
		TSharedPtr<FJsonObject> ErrorData = MakeShared<FJsonObject>();
		ErrorData->SetStringField(TEXT("error"), Code);
		ErrorData->SetStringField(TEXT("detail"), Detail);
		if (!AssetPath.IsEmpty())
		{
			ErrorData->SetStringField(TEXT("asset_path"), AssetPath);
		}
		return FMonolithActionResult::Error(Detail).WithErrorData(ErrorData);
	}

	bool IsDataflowAssetClass(const FAssetData& AssetData)
	{
		const FString ClassName = AssetData.AssetClassPath.GetAssetName().ToString();
		const FString ClassPath = AssetData.AssetClassPath.ToString();
		return ClassPath.Contains(TEXT("/Script/Dataflow"))
			|| ClassName.Contains(TEXT("Dataflow"));
	}

	TSharedPtr<FJsonObject> MakeModuleStatus(const TCHAR* ModuleName)
	{
		FModuleManager& ModuleManager = FModuleManager::Get();
		auto Status = MakeShared<FJsonObject>();
		Status->SetStringField(TEXT("name"), ModuleName);
		Status->SetBoolField(TEXT("exists"), ModuleManager.ModuleExists(ModuleName));
		Status->SetBoolField(TEXT("loaded"), ModuleManager.IsModuleLoaded(ModuleName));
		return Status;
	}

#if WITH_MONOLITH_DATAFLOW
	FString NormalizeDataflowObjectPath(const FString& RawPath)
	{
		FString Path = RawPath.TrimStartAndEnd();
		if (!Path.StartsWith(TEXT("/")))
		{
			Path = TEXT("/Game/") + Path;
		}

		if (!Path.Contains(TEXT(".")))
		{
			Path = Path + TEXT(".") + FPackageName::GetShortName(Path);
		}

		return Path;
	}

	UDataflow* LoadDataflowAsset(const TSharedPtr<FJsonObject>& Params, FString& OutRequestedPath, FString& OutObjectPath, FMonolithActionResult& OutError)
	{
		if (!Params->TryGetStringField(TEXT("asset_path"), OutRequestedPath) || OutRequestedPath.TrimStartAndEnd().IsEmpty())
		{
			OutError = ErrorWithCode(TEXT("missing_asset"), TEXT("Missing required param: asset_path"));
			return nullptr;
		}

		OutObjectPath = NormalizeDataflowObjectPath(OutRequestedPath);
		UObject* LoadedObject = StaticLoadObject(UDataflow::StaticClass(), nullptr, *OutObjectPath);
		UDataflow* Dataflow = Cast<UDataflow>(LoadedObject);
		if (!Dataflow)
		{
			OutError = ErrorWithCode(
				TEXT("missing_asset"),
				FString::Printf(TEXT("Could not load a UDataflow asset at %s"), *OutObjectPath),
				OutRequestedPath);
			return nullptr;
		}

		return Dataflow;
	}

	template <typename EnumType>
	FString EnumToString(EnumType Value)
	{
		if (const UEnum* Enum = StaticEnum<EnumType>())
		{
			return Enum->GetNameStringByValue(static_cast<int64>(Value));
		}
		return FString::FromInt(static_cast<int32>(Value));
	}

	TSharedPtr<FJsonObject> MakePositionJson(int32 X, int32 Y)
	{
		TSharedPtr<FJsonObject> Position = MakeShared<FJsonObject>();
		Position->SetNumberField(TEXT("x"), X);
		Position->SetNumberField(TEXT("y"), Y);
		return Position;
	}

	TSharedPtr<FJsonObject> MakeSizeJson(int32 Width, int32 Height)
	{
		TSharedPtr<FJsonObject> Size = MakeShared<FJsonObject>();
		Size->SetNumberField(TEXT("width"), Width);
		Size->SetNumberField(TEXT("height"), Height);
		return Size;
	}

	TSharedPtr<FJsonObject> MakeColorJson(const FLinearColor& Color)
	{
		TSharedPtr<FJsonObject> ColorJson = MakeShared<FJsonObject>();
		ColorJson->SetNumberField(TEXT("r"), Color.R);
		ColorJson->SetNumberField(TEXT("g"), Color.G);
		ColorJson->SetNumberField(TEXT("b"), Color.B);
		ColorJson->SetNumberField(TEXT("a"), Color.A);
		return ColorJson;
	}

	void AddConnectionPin(TArray<TSharedPtr<FJsonValue>>& OutPins, const FDataflowConnection* Pin)
	{
		if (!Pin)
		{
			return;
		}

		TSharedPtr<FJsonObject> PinJson = MakeShared<FJsonObject>();
		PinJson->SetStringField(TEXT("name"), Pin->GetName().ToString());
		PinJson->SetStringField(TEXT("type"), Pin->GetType().ToString());
		PinJson->SetStringField(TEXT("guid"), Pin->GetGuid().ToString());
		PinJson->SetBoolField(TEXT("connected"), Pin->IsConnected());
		PinJson->SetBoolField(TEXT("hidden"), Pin->GetPinIsHidden());
		OutPins.Add(MakeShared<FJsonValueObject>(PinJson));
	}

	void AddFallbackPin(TArray<TSharedPtr<FJsonValue>>& OutPins, const UE::Dataflow::FPin& Pin)
	{
		TSharedPtr<FJsonObject> PinJson = MakeShared<FJsonObject>();
		PinJson->SetStringField(TEXT("name"), Pin.Name.ToString());
		PinJson->SetStringField(TEXT("type"), Pin.Type.ToString());
		PinJson->SetBoolField(TEXT("hidden"), Pin.bHidden);
		OutPins.Add(MakeShared<FJsonValueObject>(PinJson));
	}

	TArray<TSharedPtr<FJsonValue>> MakeInputPins(const FDataflowNode* Node)
	{
		TArray<TSharedPtr<FJsonValue>> Pins;
		if (!Node)
		{
			return Pins;
		}

		TArray<FDataflowInput*> Inputs = Node->GetInputs();
		if (Inputs.Num() > 0)
		{
			Pins.Reserve(Inputs.Num());
			for (const FDataflowInput* Input : Inputs)
			{
				AddConnectionPin(Pins, Input);
			}
			return Pins;
		}

		for (const UE::Dataflow::FPin& Pin : Node->GetPins())
		{
			if (Pin.Direction == UE::Dataflow::FPin::EDirection::INPUT)
			{
				AddFallbackPin(Pins, Pin);
			}
		}
		return Pins;
	}

	TArray<TSharedPtr<FJsonValue>> MakeOutputPins(const FDataflowNode* Node)
	{
		TArray<TSharedPtr<FJsonValue>> Pins;
		if (!Node)
		{
			return Pins;
		}

		TArray<FDataflowOutput*> Outputs = Node->GetOutputs();
		if (Outputs.Num() > 0)
		{
			Pins.Reserve(Outputs.Num());
			for (const FDataflowOutput* Output : Outputs)
			{
				AddConnectionPin(Pins, Output);
			}
			return Pins;
		}

		for (const UE::Dataflow::FPin& Pin : Node->GetPins())
		{
			if (Pin.Direction == UE::Dataflow::FPin::EDirection::OUTPUT)
			{
				AddFallbackPin(Pins, Pin);
			}
		}
		return Pins;
	}

	TArray<TSharedPtr<FJsonValue>> MakeEditableProperties(const FDataflowNode* Node)
	{
		TArray<TSharedPtr<FJsonValue>> Properties;
		if (!Node)
		{
			return Properties;
		}

		const UScriptStruct* NodeStruct = Node->TypedScriptStruct();
		if (!NodeStruct)
		{
			return Properties;
		}

		for (TFieldIterator<FProperty> PropIt(NodeStruct, EFieldIteratorFlags::IncludeSuper); PropIt; ++PropIt)
		{
			const FProperty* Property = *PropIt;
			if (!Property || !Property->HasAnyPropertyFlags(CPF_Edit))
			{
				continue;
			}
			if (Property->HasAnyPropertyFlags(CPF_Deprecated | CPF_Transient))
			{
				continue;
			}
			if (Property->GetOwnerStruct() == FDataflowNode::StaticStruct())
			{
				continue;
			}

			FString Value;
			const void* ValuePtr = Property->ContainerPtrToValuePtr<const void>(Node);
			Property->ExportTextItem_Direct(Value, ValuePtr, nullptr, nullptr, PPF_None);

			TSharedPtr<FJsonObject> PropJson = MakeShared<FJsonObject>();
			PropJson->SetStringField(TEXT("name"), Property->GetName());
			PropJson->SetStringField(TEXT("type"), Property->GetCPPType());
			PropJson->SetStringField(TEXT("value"), Value);
			PropJson->SetBoolField(TEXT("read_only"), Property->HasAnyPropertyFlags(CPF_EditConst));
			Properties.Add(MakeShared<FJsonValueObject>(PropJson));
		}

		return Properties;
	}

	TSharedPtr<FJsonObject> MakeNodeJson(const UDataflow* Dataflow, const FDataflowNode* Node, bool bIncludeProperties)
	{
		TSharedPtr<FJsonObject> NodeJson = MakeShared<FJsonObject>();
		if (!Node)
		{
			return NodeJson;
		}

		NodeJson->SetStringField(TEXT("name"), Node->GetName().ToString());
		NodeJson->SetStringField(TEXT("guid"), Node->GetGuid().ToString());
		NodeJson->SetStringField(TEXT("type"), Node->GetType().ToString());
		NodeJson->SetStringField(TEXT("display_name"), Node->GetDisplayName().ToString());
		NodeJson->SetStringField(TEXT("category"), Node->GetCategory().ToString());
		NodeJson->SetStringField(TEXT("tags"), Node->GetTags());
		NodeJson->SetStringField(TEXT("tooltip"), Node->GetToolTip());

		if (Dataflow)
		{
			if (const TObjectPtr<const UDataflowEdNode> EdNode = Dataflow->FindEdNodeByDataflowNodeGuid(Node->GetGuid()))
			{
				TSharedPtr<FJsonObject> Position = MakeShared<FJsonObject>();
				Position->SetNumberField(TEXT("x"), EdNode->NodePosX);
				Position->SetNumberField(TEXT("y"), EdNode->NodePosY);
				NodeJson->SetObjectField(TEXT("position"), Position);
			}
		}

		NodeJson->SetArrayField(TEXT("input_pins"), MakeInputPins(Node));
		NodeJson->SetArrayField(TEXT("output_pins"), MakeOutputPins(Node));
		if (bIncludeProperties)
		{
			NodeJson->SetArrayField(TEXT("properties"), MakeEditableProperties(Node));
		}

		return NodeJson;
	}

	TSharedPtr<FJsonObject> MakeConnectionJson(const UE::Dataflow::FGraph& Graph, const UE::Dataflow::FLink& Link)
	{
		TSharedPtr<FJsonObject> LinkJson = MakeShared<FJsonObject>();
		LinkJson->SetStringField(TEXT("from_node_guid"), Link.OutputNode.ToString());
		LinkJson->SetStringField(TEXT("from_pin_guid"), Link.Output.ToString());
		LinkJson->SetStringField(TEXT("to_node_guid"), Link.InputNode.ToString());
		LinkJson->SetStringField(TEXT("to_pin_guid"), Link.Input.ToString());

		if (TSharedPtr<const FDataflowNode> FromNode = Graph.FindBaseNode(Link.OutputNode))
		{
			LinkJson->SetStringField(TEXT("from_node"), FromNode->GetName().ToString());
			if (const FDataflowOutput* OutputPin = FromNode->FindOutput(Link.Output))
			{
				LinkJson->SetStringField(TEXT("from_pin"), OutputPin->GetName().ToString());
				LinkJson->SetStringField(TEXT("from_pin_type"), OutputPin->GetType().ToString());
			}
		}

		if (TSharedPtr<const FDataflowNode> ToNode = Graph.FindBaseNode(Link.InputNode))
		{
			LinkJson->SetStringField(TEXT("to_node"), ToNode->GetName().ToString());
			if (const FDataflowInput* InputPin = ToNode->FindInput(Link.Input))
			{
				LinkJson->SetStringField(TEXT("to_pin"), InputPin->GetName().ToString());
				LinkJson->SetStringField(TEXT("to_pin_type"), InputPin->GetType().ToString());
			}
		}

		return LinkJson;
	}

	TSharedPtr<FJsonObject> MakeFactoryParamsJson(const UE::Dataflow::FFactoryParameters& FactoryParams, bool bIncludePins, bool bIncludeProperties)
	{
		TSharedPtr<FJsonObject> NodeType = MakeShared<FJsonObject>();
		NodeType->SetStringField(TEXT("type_name"), FactoryParams.TypeName.ToString());
		NodeType->SetStringField(TEXT("display_name"), FactoryParams.DisplayName.ToString());
		NodeType->SetStringField(TEXT("category"), FactoryParams.Category.ToString());
		NodeType->SetStringField(TEXT("tags"), FactoryParams.Tags);
		NodeType->SetStringField(TEXT("tooltip"), FactoryParams.ToolTip);
		NodeType->SetStringField(TEXT("version"), FactoryParams.GetVersion().ToString());
		NodeType->SetBoolField(TEXT("deprecated"), FactoryParams.IsDeprecated());
		NodeType->SetBoolField(TEXT("experimental"), FactoryParams.IsExperimental());

		if (FactoryParams.DefaultNodeObject.IsValid())
		{
			const FDataflowNode* DefaultNode = FactoryParams.DefaultNodeObject.Get();
			if (bIncludePins)
			{
				NodeType->SetArrayField(TEXT("input_pins"), MakeInputPins(DefaultNode));
				NodeType->SetArrayField(TEXT("output_pins"), MakeOutputPins(DefaultNode));
			}
			if (bIncludeProperties)
			{
				NodeType->SetArrayField(TEXT("properties"), MakeEditableProperties(DefaultNode));
			}
		}

		return NodeType;
	}

	void AddValidationIssue(TArray<TSharedPtr<FJsonValue>>& Issues, const FString& Code, const FString& Detail, const FString& Guid = FString())
	{
		TSharedPtr<FJsonObject> Issue = MakeShared<FJsonObject>();
		Issue->SetStringField(TEXT("code"), Code);
		Issue->SetStringField(TEXT("detail"), Detail);
		if (!Guid.IsEmpty())
		{
			Issue->SetStringField(TEXT("guid"), Guid);
		}
		Issues.Add(MakeShared<FJsonValueObject>(Issue));
	}

	TArray<TSharedPtr<FJsonValue>> MakeContainerTypeList(const FPropertyBagContainerTypes& ContainerTypes)
	{
		TArray<TSharedPtr<FJsonValue>> Rows;
		Rows.Reserve(ContainerTypes.Num());
		for (const EPropertyBagContainerType ContainerType : ContainerTypes)
		{
			Rows.Add(MakeShared<FJsonValueString>(EnumToString(ContainerType)));
		}
		return Rows;
	}

	TSharedPtr<FJsonObject> MakeDataflowVariableJson(const UDataflow* Dataflow, const FPropertyBagPropertyDesc& Desc)
	{
		TSharedPtr<FJsonObject> Variable = MakeShared<FJsonObject>();
		Variable->SetStringField(TEXT("name"), Desc.Name.ToString());
		Variable->SetStringField(TEXT("guid"), Desc.ID.ToString());
		Variable->SetStringField(TEXT("value_type"), EnumToString(Desc.ValueType));
		Variable->SetStringField(TEXT("container_type"), EnumToString(Desc.ContainerTypes.GetFirstContainerType()));
		Variable->SetNumberField(TEXT("container_depth"), static_cast<int32>(Desc.ContainerTypes.Num()));
		Variable->SetArrayField(TEXT("container_types"), MakeContainerTypeList(Desc.ContainerTypes));
		Variable->SetBoolField(TEXT("is_container"), !Desc.ContainerTypes.IsEmpty());
		Variable->SetBoolField(TEXT("read_only"), (Desc.PropertyFlags & CPF_EditConst) != 0);

		if (Desc.ValueTypeObject)
		{
			Variable->SetStringField(TEXT("value_type_object"), Desc.ValueTypeObject->GetName());
			Variable->SetStringField(TEXT("value_type_object_path"), Desc.ValueTypeObject->GetPathName());
			Variable->SetStringField(TEXT("value_type_object_class"), Desc.ValueTypeObject->GetClass()->GetName());
		}

		const TValueOrError<FString, EPropertyBagResult> SerializedValue = Dataflow->Variables.GetValueSerializedString(Desc.Name);
		if (SerializedValue.HasValue())
		{
			Variable->SetStringField(TEXT("value"), SerializedValue.GetValue());
			Variable->SetStringField(TEXT("value_read_status"), TEXT("ok"));
		}
		else if (SerializedValue.HasError())
		{
			Variable->SetStringField(TEXT("value_read_status"), EnumToString(SerializedValue.GetError()));
		}
		else
		{
			Variable->SetStringField(TEXT("value_read_status"), TEXT("unavailable"));
		}

		return Variable;
	}

	bool IsNodeInsideComment(const UEdGraphNode_Comment* Comment, const UEdGraphNode* Node)
	{
		if (!Comment || !Node || Node == Comment)
		{
			return false;
		}

		const int32 CommentRight = Comment->NodePosX + Comment->NodeWidth;
		const int32 CommentBottom = Comment->NodePosY + Comment->NodeHeight;
		return Node->NodePosX >= Comment->NodePosX
			&& Node->NodePosY >= Comment->NodePosY
			&& Node->NodePosX <= CommentRight
			&& Node->NodePosY <= CommentBottom;
	}

	TSharedPtr<FJsonObject> MakeContainedEdNodeJson(const UEdGraphNode* Node)
	{
		TSharedPtr<FJsonObject> NodeJson = MakeShared<FJsonObject>();
		if (!Node)
		{
			return NodeJson;
		}

		NodeJson->SetStringField(TEXT("guid"), Node->NodeGuid.ToString());
		NodeJson->SetStringField(TEXT("name"), Node->GetName());
		NodeJson->SetStringField(TEXT("class"), Node->GetClass()->GetName());
		NodeJson->SetObjectField(TEXT("position"), MakePositionJson(Node->NodePosX, Node->NodePosY));

		if (const UDataflowEdNode* DataflowEdNode = Cast<UDataflowEdNode>(Node))
		{
			NodeJson->SetStringField(TEXT("dataflow_node_guid"), DataflowEdNode->GetDataflowNodeGuid().ToString());
			if (const TSharedPtr<const FDataflowNode> DataflowNode = DataflowEdNode->GetDataflowNode())
			{
				NodeJson->SetStringField(TEXT("dataflow_node_name"), DataflowNode->GetName().ToString());
				NodeJson->SetStringField(TEXT("dataflow_node_type"), DataflowNode->GetType().ToString());
			}
		}

		return NodeJson;
	}

	TSharedPtr<FJsonObject> MakeCommentJson(const UEdGraphNode_Comment* Comment, const TArray<const UEdGraphNode*>& GraphNodes, int32 NodeLimit)
	{
		TSharedPtr<FJsonObject> CommentJson = MakeShared<FJsonObject>();
		if (!Comment)
		{
			return CommentJson;
		}

		TArray<TSharedPtr<FJsonValue>> ContainedNodes;
		int32 TotalContained = 0;
		for (const UEdGraphNode* Node : GraphNodes)
		{
			if (IsNodeInsideComment(Comment, Node))
			{
				++TotalContained;
				if (ContainedNodes.Num() < NodeLimit)
				{
					ContainedNodes.Add(MakeShared<FJsonValueObject>(MakeContainedEdNodeJson(Node)));
				}
			}
		}
		const bool bContainedTruncated = TotalContained > ContainedNodes.Num();

		CommentJson->SetStringField(TEXT("guid"), Comment->NodeGuid.ToString());
		CommentJson->SetStringField(TEXT("comment"), Comment->NodeComment);
		CommentJson->SetStringField(TEXT("details"), Comment->NodeDetails.ToString());
		CommentJson->SetStringField(TEXT("class"), Comment->GetClass()->GetName());
		CommentJson->SetObjectField(TEXT("position"), MakePositionJson(Comment->NodePosX, Comment->NodePosY));
		CommentJson->SetObjectField(TEXT("size"), MakeSizeJson(Comment->NodeWidth, Comment->NodeHeight));
		CommentJson->SetObjectField(TEXT("color"), MakeColorJson(Comment->CommentColor));
		CommentJson->SetNumberField(TEXT("font_size"), Comment->FontSize);
		CommentJson->SetStringField(TEXT("move_mode"), Comment->MoveMode == ECommentBoxMode::GroupMovement ? TEXT("GroupMovement") : TEXT("NoGroupMovement"));
		CommentJson->SetNumberField(TEXT("comment_depth"), Comment->CommentDepth);
		CommentJson->SetNumberField(TEXT("contained_node_count"), TotalContained);
		CommentJson->SetBoolField(TEXT("contained_nodes_truncated"), bContainedTruncated);
		CommentJson->SetArrayField(TEXT("contained_nodes"), ContainedNodes);
		return CommentJson;
	}
#endif
}

void FMonolithDataflowActions::RegisterActions(FMonolithToolRegistry& Registry)
{
	Registry.RegisterAction(TEXT("dataflow"), TEXT("get_status"),
		TEXT("Report read-only Dataflow/Chaos graph discovery support without adding hard Dataflow link dependencies."),
		FMonolithActionHandler::CreateStatic(&FMonolithDataflowActions::GetStatus),
		FParamSchemaBuilder().Build());

	Registry.RegisterAction(TEXT("dataflow"), TEXT("list_assets"),
		TEXT("List Dataflow asset metadata under /Game using AssetRegistry only. Does not load, evaluate, regenerate, or mutate assets."),
		FMonolithActionHandler::CreateStatic(&FMonolithDataflowActions::ListAssets),
		FParamSchemaBuilder()
			.Optional(TEXT("package_path"), TEXT("string"), TEXT("Content package path under /Game"), TEXT("/Game"))
			.Optional(TEXT("limit"), TEXT("integer"), TEXT("Maximum assets to return, clamped to 1..500"), TEXT("100"))
			.Build());

#if WITH_MONOLITH_DATAFLOW
	Registry.RegisterAction(TEXT("dataflow"), TEXT("get_dataflow_graph"),
		TEXT("Read a bounded Dataflow graph summary from a UDataflow asset. Does not mutate, evaluate, regenerate, or mark packages dirty."),
		FMonolithActionHandler::CreateStatic(&FMonolithDataflowActions::GetDataflowGraph),
		FParamSchemaBuilder()
			.Required(TEXT("asset_path"), TEXT("string"), TEXT("Dataflow asset path, e.g. /Game/Geometry/DF_Fracture"))
			.Optional(TEXT("node_limit"), TEXT("integer"), TEXT("Maximum node rows to return, clamped to 1..500"), TEXT("128"))
			.Optional(TEXT("connection_limit"), TEXT("integer"), TEXT("Maximum connection rows to return after node_limit filtering, clamped to 1..5000"), TEXT("1000"))
			.Optional(TEXT("include_properties"), TEXT("boolean"), TEXT("Include editable UPROPERTY snapshots for each returned node"), TEXT("false"))
			.Build(),
		TEXT("Dataflow"));

	Registry.RegisterAction(TEXT("dataflow"), TEXT("list_dataflow_node_types"),
		TEXT("List registered Dataflow node factory types with optional filtering and pin summaries."),
		FMonolithActionHandler::CreateStatic(&FMonolithDataflowActions::ListDataflowNodeTypes),
		FParamSchemaBuilder()
			.Optional(TEXT("filter"), TEXT("string"), TEXT("Substring filter across type, display name, category, and tags"))
			.Optional(TEXT("common_only"), TEXT("boolean"), TEXT("Exclude deprecated and experimental node types"), TEXT("true"))
			.Optional(TEXT("limit"), TEXT("integer"), TEXT("Maximum type rows to return, clamped to 1..1000"), TEXT("200"))
			.Optional(TEXT("include_pins"), TEXT("boolean"), TEXT("Include default input/output pins for each returned type"), TEXT("false"))
			.Build(),
		TEXT("Dataflow"));

	Registry.RegisterAction(TEXT("dataflow"), TEXT("get_dataflow_node_schema"),
		TEXT("Return schema details for one registered Dataflow node type."),
		FMonolithActionHandler::CreateStatic(&FMonolithDataflowActions::GetDataflowNodeSchema),
		FParamSchemaBuilder()
			.Required(TEXT("type_name"), TEXT("string"), TEXT("Registered Dataflow node type name, e.g. FAddFloatsDataflowNode"))
			.Optional(TEXT("include_properties"), TEXT("boolean"), TEXT("Include editable UPROPERTY defaults for this node type"), TEXT("true"))
			.Build(),
		TEXT("Dataflow"));

	Registry.RegisterAction(TEXT("dataflow"), TEXT("validate_dataflow_graph"),
		TEXT("Validate a Dataflow graph for duplicate node identifiers and broken connection references without mutation."),
		FMonolithActionHandler::CreateStatic(&FMonolithDataflowActions::ValidateDataflowGraph),
		FParamSchemaBuilder()
			.Required(TEXT("asset_path"), TEXT("string"), TEXT("Dataflow asset path, e.g. /Game/Geometry/DF_Fracture"))
			.Build(),
		TEXT("Dataflow"));

	Registry.RegisterAction(TEXT("dataflow"), TEXT("list_dataflow_variables"),
		TEXT("List UDataflow property bag variables with descriptor metadata and serialized values without mutation."),
		FMonolithActionHandler::CreateStatic(&FMonolithDataflowActions::ListDataflowVariables),
		FParamSchemaBuilder()
			.Required(TEXT("asset_path"), TEXT("string"), TEXT("Dataflow asset path, e.g. /Game/Geometry/DF_Fracture"))
			.Build(),
		TEXT("Dataflow"));

	Registry.RegisterAction(TEXT("dataflow"), TEXT("list_dataflow_comments"),
		TEXT("List Dataflow editor comment boxes with bounded node membership hints without mutation."),
		FMonolithActionHandler::CreateStatic(&FMonolithDataflowActions::ListDataflowComments),
		FParamSchemaBuilder()
			.Required(TEXT("asset_path"), TEXT("string"), TEXT("Dataflow asset path, e.g. /Game/Geometry/DF_Fracture"))
			.Optional(TEXT("node_limit"), TEXT("integer"), TEXT("Maximum contained-node rows per comment, clamped to 1..500"), TEXT("128"))
			.Build(),
		TEXT("Dataflow"));
#endif
}

FMonolithActionResult FMonolithDataflowActions::GetStatus(const TSharedPtr<FJsonObject>& Params)
{
	auto Result = MakeShared<FJsonObject>();
	Result->SetStringField(TEXT("namespace"), TEXT("dataflow"));
	Result->SetStringField(TEXT("domain"), TEXT("dataflow_discovery"));
	Result->SetStringField(TEXT("mode"), TEXT("read_only"));
	Result->SetBoolField(TEXT("hard_dependency"), false);
	Result->SetBoolField(TEXT("graph_inspection_compiled"), WITH_MONOLITH_DATAFLOW != 0);

	TArray<TSharedPtr<FJsonValue>> Modules;
	Modules.Reserve(6);
	Modules.Add(MakeShared<FJsonValueObject>(MonolithDataflow::MakeModuleStatus(TEXT("DataflowCore"))));
	Modules.Add(MakeShared<FJsonValueObject>(MonolithDataflow::MakeModuleStatus(TEXT("DataflowEngine"))));
	Modules.Add(MakeShared<FJsonValueObject>(MonolithDataflow::MakeModuleStatus(TEXT("DataflowEditor"))));
	Modules.Add(MakeShared<FJsonValueObject>(MonolithDataflow::MakeModuleStatus(TEXT("DataflowNodes"))));
	Modules.Add(MakeShared<FJsonValueObject>(MonolithDataflow::MakeModuleStatus(TEXT("GeometryCollectionEngine"))));
	Modules.Add(MakeShared<FJsonValueObject>(MonolithDataflow::MakeModuleStatus(TEXT("ChaosCaching"))));
	Result->SetArrayField(TEXT("modules"), Modules);

	TArray<TSharedPtr<FJsonValue>> ImplementedActions;
#if WITH_MONOLITH_DATAFLOW
	ImplementedActions.Reserve(8);
#else
	ImplementedActions.Reserve(2);
#endif
	ImplementedActions.Add(MakeShared<FJsonValueString>(TEXT("dataflow.get_status")));
	ImplementedActions.Add(MakeShared<FJsonValueString>(TEXT("dataflow.list_assets")));
#if WITH_MONOLITH_DATAFLOW
	ImplementedActions.Add(MakeShared<FJsonValueString>(TEXT("dataflow.get_dataflow_graph")));
	ImplementedActions.Add(MakeShared<FJsonValueString>(TEXT("dataflow.list_dataflow_node_types")));
	ImplementedActions.Add(MakeShared<FJsonValueString>(TEXT("dataflow.get_dataflow_node_schema")));
	ImplementedActions.Add(MakeShared<FJsonValueString>(TEXT("dataflow.validate_dataflow_graph")));
	ImplementedActions.Add(MakeShared<FJsonValueString>(TEXT("dataflow.list_dataflow_variables")));
	ImplementedActions.Add(MakeShared<FJsonValueString>(TEXT("dataflow.list_dataflow_comments")));
#endif
	Result->SetArrayField(TEXT("implemented_actions"), ImplementedActions);

	TArray<TSharedPtr<FJsonValue>> FutureActions;
#if !WITH_MONOLITH_DATAFLOW
	FutureActions.Reserve(9);
	FutureActions.Add(MakeShared<FJsonValueString>(TEXT("dataflow.get_dataflow_graph")));
	FutureActions.Add(MakeShared<FJsonValueString>(TEXT("dataflow.list_dataflow_node_types")));
	FutureActions.Add(MakeShared<FJsonValueString>(TEXT("dataflow.get_dataflow_node_schema")));
	FutureActions.Add(MakeShared<FJsonValueString>(TEXT("dataflow.validate_dataflow_graph")));
	FutureActions.Add(MakeShared<FJsonValueString>(TEXT("dataflow.list_dataflow_variables")));
	FutureActions.Add(MakeShared<FJsonValueString>(TEXT("dataflow.list_dataflow_comments")));
#else
	FutureActions.Reserve(3);
#endif
	FutureActions.Add(MakeShared<FJsonValueString>(TEXT("dataflow.can_connect_dataflow")));
	FutureActions.Add(MakeShared<FJsonValueString>(TEXT("dataflow.evaluate_dataflow_terminal")));
	FutureActions.Add(MakeShared<FJsonValueString>(TEXT("dataflow.regenerate_dataflow")));
	Result->SetArrayField(TEXT("future_optional_actions"), FutureActions);

	TArray<TSharedPtr<FJsonValue>> Notes;
	Notes.Reserve(3);
	Notes.Add(MakeShared<FJsonValueString>(TEXT("MonolithDataflow owns the dataflow namespace; AssetRegistry discovery is always dependency-light.")));
#if WITH_MONOLITH_DATAFLOW
	Notes.Add(MakeShared<FJsonValueString>(TEXT("Graph inspection is compiled because DataflowCore/DataflowEngine headers are available and MONOLITH_RELEASE_BUILD is not set.")));
#else
	Notes.Add(MakeShared<FJsonValueString>(TEXT("Graph inspection actions are omitted because Dataflow runtime headers are unavailable or MONOLITH_RELEASE_BUILD=1 forced optional dependencies off.")));
#endif
	Notes.Add(MakeShared<FJsonValueString>(TEXT("Graph actions are read-only; mutation, evaluation, regeneration, and pin writes remain future optional dataflow work.")));
	Result->SetArrayField(TEXT("notes"), Notes);

	return FMonolithActionResult::Success(Result);
}

FMonolithActionResult FMonolithDataflowActions::ListAssets(const TSharedPtr<FJsonObject>& Params)
{
	FString PackagePath = TEXT("/Game");
	Params->TryGetStringField(TEXT("package_path"), PackagePath);
	if (!PackagePath.StartsWith(TEXT("/Game")))
	{
		return FMonolithActionResult::Error(TEXT("package_path must be under /Game"));
	}

	double LimitValue = 100.0;
	Params->TryGetNumberField(TEXT("limit"), LimitValue);
	const int32 Limit = MonolithDataflow::ClampLimit(LimitValue);

	IAssetRegistry& AssetRegistry = FModuleManager::LoadModuleChecked<FAssetRegistryModule>(TEXT("AssetRegistry")).Get();
	FARFilter Filter;
	Filter.PackagePaths.Add(FName(*PackagePath));
	Filter.bRecursivePaths = true;

	TArray<FAssetData> Assets;
	AssetRegistry.GetAssets(Filter, Assets);

	TArray<TSharedPtr<FJsonValue>> Rows;
	Rows.Reserve(FMath::Min(Assets.Num(), Limit));
	int32 MatchedCount = 0;
	TMap<FString, int32> ClassCounts;

	for (const FAssetData& AssetData : Assets)
	{
		if (!MonolithDataflow::IsDataflowAssetClass(AssetData))
		{
			continue;
		}

		MatchedCount++;
		const FString ClassName = AssetData.AssetClassPath.GetAssetName().ToString();
		ClassCounts.FindOrAdd(ClassName)++;

		if (Rows.Num() >= Limit)
		{
			continue;
		}

		auto Row = MakeShared<FJsonObject>();
		Row->SetStringField(TEXT("object_path"), AssetData.GetObjectPathString());
		Row->SetStringField(TEXT("package_name"), AssetData.PackageName.ToString());
		Row->SetStringField(TEXT("package_path"), AssetData.PackagePath.ToString());
		Row->SetStringField(TEXT("asset_name"), AssetData.AssetName.ToString());
		Row->SetStringField(TEXT("asset_class"), ClassName);
		Row->SetStringField(TEXT("asset_class_path"), AssetData.AssetClassPath.ToString());
		Row->SetBoolField(TEXT("loaded"), AssetData.IsAssetLoaded());
		Rows.Add(MakeShared<FJsonValueObject>(Row));
	}

	auto CountsJson = MakeShared<FJsonObject>();
	for (const TPair<FString, int32>& Pair : ClassCounts)
	{
		CountsJson->SetNumberField(Pair.Key, Pair.Value);
	}

	auto Result = MakeShared<FJsonObject>();
	Result->SetStringField(TEXT("namespace"), TEXT("dataflow"));
	Result->SetStringField(TEXT("domain"), TEXT("dataflow_discovery"));
	Result->SetStringField(TEXT("package_path"), PackagePath);
	Result->SetNumberField(TEXT("matched_count"), MatchedCount);
	Result->SetNumberField(TEXT("returned_count"), Rows.Num());
	Result->SetNumberField(TEXT("limit"), Limit);
	Result->SetBoolField(TEXT("truncated"), MatchedCount > Rows.Num());
	Result->SetObjectField(TEXT("class_counts"), CountsJson);
	Result->SetArrayField(TEXT("assets"), Rows);
	return FMonolithActionResult::Success(Result);
}

#if WITH_MONOLITH_DATAFLOW
FMonolithActionResult FMonolithDataflowActions::GetDataflowGraph(const TSharedPtr<FJsonObject>& Params)
{
	FString RequestedPath;
	FString ObjectPath;
	FMonolithActionResult Error;
	UDataflow* Dataflow = MonolithDataflow::LoadDataflowAsset(Params, RequestedPath, ObjectPath, Error);
	if (!Dataflow)
	{
		return Error;
	}

	const auto Graph = Dataflow->GetDataflow();
	if (!Graph.IsValid())
	{
		return MonolithDataflow::ErrorWithCode(
			TEXT("unsupported_graph_type"),
			FString::Printf(TEXT("Dataflow asset has no graph: %s"), *ObjectPath),
			RequestedPath);
	}

	const int32 NodeLimit = MonolithDataflow::GetClampedIntParam(Params, TEXT("node_limit"), 128, 1, 500);
	const int32 ConnectionLimit = MonolithDataflow::GetClampedIntParam(Params, TEXT("connection_limit"), 1000, 1, 5000);
	const bool bIncludeProperties = MonolithDataflow::GetBoolParam(Params, TEXT("include_properties"), false);

	const TArray<TSharedPtr<FDataflowNode>>& Nodes = Graph->GetNodes();
	TArray<TSharedPtr<FJsonValue>> NodeRows;
	TSet<FGuid> ReturnedNodeGuids;
	NodeRows.Reserve(FMath::Min(Nodes.Num(), NodeLimit));
	for (int32 Index = 0; Index < Nodes.Num() && NodeRows.Num() < NodeLimit; ++Index)
	{
		if (Nodes[Index].IsValid())
		{
			ReturnedNodeGuids.Add(Nodes[Index]->GetGuid());
			NodeRows.Add(MakeShared<FJsonValueObject>(MonolithDataflow::MakeNodeJson(Dataflow, Nodes[Index].Get(), bIncludeProperties)));
		}
	}

	TArray<TSharedPtr<FJsonValue>> ConnectionRows;
	const TArray<UE::Dataflow::FLink>& Links = Graph->GetConnections();
	int32 MatchingConnectionCount = 0;
	ConnectionRows.Reserve(FMath::Min(Links.Num(), ConnectionLimit));
	for (const UE::Dataflow::FLink& Link : Links)
	{
		if (!ReturnedNodeGuids.Contains(Link.OutputNode) || !ReturnedNodeGuids.Contains(Link.InputNode))
		{
			continue;
		}

		MatchingConnectionCount++;
		if (ConnectionRows.Num() >= ConnectionLimit)
		{
			continue;
		}
		ConnectionRows.Add(MakeShared<FJsonValueObject>(MonolithDataflow::MakeConnectionJson(*Graph, Link)));
	}

	TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
	Result->SetStringField(TEXT("namespace"), TEXT("dataflow"));
	Result->SetStringField(TEXT("domain"), TEXT("dataflow_graph"));
	Result->SetStringField(TEXT("asset_path"), RequestedPath);
	Result->SetStringField(TEXT("object_path"), ObjectPath);
	Result->SetBoolField(TEXT("read_only"), true);
	Result->SetBoolField(TEXT("active"), Dataflow->bActive);
	Result->SetStringField(TEXT("dataflow_type"), StaticEnum<EDataflowType>() ? StaticEnum<EDataflowType>()->GetNameStringByValue(static_cast<int64>(Dataflow->Type)) : FString::FromInt(static_cast<int32>(Dataflow->Type)));
	Result->SetNumberField(TEXT("node_count"), Nodes.Num());
	Result->SetNumberField(TEXT("returned_node_count"), NodeRows.Num());
	Result->SetNumberField(TEXT("node_limit"), NodeLimit);
	Result->SetNumberField(TEXT("connection_count"), Links.Num());
	Result->SetNumberField(TEXT("matching_connection_count"), MatchingConnectionCount);
	Result->SetNumberField(TEXT("returned_connection_count"), ConnectionRows.Num());
	Result->SetNumberField(TEXT("connection_limit"), ConnectionLimit);
	Result->SetBoolField(TEXT("nodes_truncated"), Nodes.Num() > NodeRows.Num());
	Result->SetBoolField(TEXT("connections_truncated"), Links.Num() > ConnectionRows.Num());
	Result->SetBoolField(TEXT("truncated"), Nodes.Num() > NodeRows.Num() || Links.Num() > ConnectionRows.Num());
	Result->SetArrayField(TEXT("nodes"), NodeRows);
	Result->SetArrayField(TEXT("connections"), ConnectionRows);
	return FMonolithActionResult::Success(Result);
}

FMonolithActionResult FMonolithDataflowActions::ListDataflowNodeTypes(const TSharedPtr<FJsonObject>& Params)
{
	UE::Dataflow::FNodeFactory* Factory = UE::Dataflow::FNodeFactory::GetInstance();
	if (!Factory)
	{
		return MonolithDataflow::ErrorWithCode(TEXT("dependency_unavailable"), TEXT("Dataflow node factory is not available"));
	}

	FString Filter;
	Params->TryGetStringField(TEXT("filter"), Filter);
	const bool bCommonOnly = MonolithDataflow::GetBoolParam(Params, TEXT("common_only"), true);
	const bool bIncludePins = MonolithDataflow::GetBoolParam(Params, TEXT("include_pins"), false);
	const int32 Limit = MonolithDataflow::GetClampedIntParam(Params, TEXT("limit"), 200, 1, 1000);

	TArray<UE::Dataflow::FFactoryParameters> RegisteredParams = Factory->RegisteredParameters();
	RegisteredParams.Sort([](const UE::Dataflow::FFactoryParameters& A, const UE::Dataflow::FFactoryParameters& B)
	{
		const int32 CategoryCompare = A.Category.ToString().Compare(B.Category.ToString());
		return CategoryCompare == 0 ? A.TypeName.ToString() < B.TypeName.ToString() : CategoryCompare < 0;
	});

	TArray<TSharedPtr<FJsonValue>> Rows;
	Rows.Reserve(FMath::Min(RegisteredParams.Num(), Limit));
	int32 MatchedCount = 0;
	for (const UE::Dataflow::FFactoryParameters& FactoryParams : RegisteredParams)
	{
		if (!FactoryParams.IsValid())
		{
			continue;
		}
		if (bCommonOnly && (FactoryParams.IsDeprecated() || FactoryParams.IsExperimental()))
		{
			continue;
		}
		if (!Filter.IsEmpty())
		{
			const FString Haystack = FString::Printf(TEXT("%s\n%s\n%s\n%s"),
				*FactoryParams.TypeName.ToString(),
				*FactoryParams.DisplayName.ToString(),
				*FactoryParams.Category.ToString(),
				*FactoryParams.Tags);
			if (!Haystack.Contains(Filter, ESearchCase::IgnoreCase))
			{
				continue;
			}
		}

		MatchedCount++;
		if (Rows.Num() < Limit)
		{
			Rows.Add(MakeShared<FJsonValueObject>(MonolithDataflow::MakeFactoryParamsJson(FactoryParams, bIncludePins, false)));
		}
	}

	TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
	Result->SetStringField(TEXT("namespace"), TEXT("dataflow"));
	Result->SetStringField(TEXT("domain"), TEXT("dataflow_graph"));
	Result->SetStringField(TEXT("filter"), Filter);
	Result->SetBoolField(TEXT("common_only"), bCommonOnly);
	Result->SetBoolField(TEXT("include_pins"), bIncludePins);
	Result->SetNumberField(TEXT("matched_count"), MatchedCount);
	Result->SetNumberField(TEXT("returned_count"), Rows.Num());
	Result->SetNumberField(TEXT("limit"), Limit);
	Result->SetBoolField(TEXT("truncated"), MatchedCount > Rows.Num());
	Result->SetArrayField(TEXT("node_types"), Rows);
	return FMonolithActionResult::Success(Result);
}

FMonolithActionResult FMonolithDataflowActions::GetDataflowNodeSchema(const TSharedPtr<FJsonObject>& Params)
{
	FString TypeName;
	if (!Params->TryGetStringField(TEXT("type_name"), TypeName) || TypeName.TrimStartAndEnd().IsEmpty())
	{
		return MonolithDataflow::ErrorWithCode(TEXT("unknown_node_type"), TEXT("Missing required param: type_name"));
	}

	UE::Dataflow::FNodeFactory* Factory = UE::Dataflow::FNodeFactory::GetInstance();
	if (!Factory)
	{
		return MonolithDataflow::ErrorWithCode(TEXT("dependency_unavailable"), TEXT("Dataflow node factory is not available"));
	}

	const UE::Dataflow::FFactoryParameters& FactoryParams = Factory->GetParameters(FName(*TypeName));
	if (!FactoryParams.IsValid())
	{
		return MonolithDataflow::ErrorWithCode(
			TEXT("unknown_node_type"),
			FString::Printf(TEXT("Unknown Dataflow node type: %s"), *TypeName));
	}

	const bool bIncludeProperties = MonolithDataflow::GetBoolParam(Params, TEXT("include_properties"), true);
	TSharedPtr<FJsonObject> Result = MonolithDataflow::MakeFactoryParamsJson(FactoryParams, true, bIncludeProperties);
	Result->SetStringField(TEXT("namespace"), TEXT("dataflow"));
	Result->SetStringField(TEXT("domain"), TEXT("dataflow_graph"));
	return FMonolithActionResult::Success(Result);
}

FMonolithActionResult FMonolithDataflowActions::ValidateDataflowGraph(const TSharedPtr<FJsonObject>& Params)
{
	FString RequestedPath;
	FString ObjectPath;
	FMonolithActionResult Error;
	UDataflow* Dataflow = MonolithDataflow::LoadDataflowAsset(Params, RequestedPath, ObjectPath, Error);
	if (!Dataflow)
	{
		return Error;
	}

	const auto Graph = Dataflow->GetDataflow();
	if (!Graph.IsValid())
	{
		return MonolithDataflow::ErrorWithCode(
			TEXT("unsupported_graph_type"),
			FString::Printf(TEXT("Dataflow asset has no graph: %s"), *ObjectPath),
			RequestedPath);
	}

	TArray<TSharedPtr<FJsonValue>> Issues;
	TSet<FName> NodeNames;
	NodeNames.Reserve(Graph->GetNodes().Num());
	TSet<FGuid> NodeGuids;
	NodeGuids.Reserve(Graph->GetNodes().Num());

	for (const TSharedPtr<FDataflowNode>& Node : Graph->GetNodes())
	{
		if (!Node.IsValid())
		{
			continue;
		}

		if (NodeNames.Contains(Node->GetName()))
		{
			MonolithDataflow::AddValidationIssue(
				Issues,
				TEXT("duplicate_node_name"),
				FString::Printf(TEXT("Duplicate Dataflow node name: %s"), *Node->GetName().ToString()),
				Node->GetGuid().ToString());
		}
		NodeNames.Add(Node->GetName());

		if (NodeGuids.Contains(Node->GetGuid()))
		{
			MonolithDataflow::AddValidationIssue(
				Issues,
				TEXT("duplicate_node_guid"),
				FString::Printf(TEXT("Duplicate Dataflow node GUID: %s"), *Node->GetGuid().ToString()),
				Node->GetGuid().ToString());
		}
		NodeGuids.Add(Node->GetGuid());
	}

	for (const UE::Dataflow::FLink& Link : Graph->GetConnections())
	{
		TSharedPtr<const FDataflowNode> FromNode = Graph->FindBaseNode(Link.OutputNode);
		TSharedPtr<const FDataflowNode> ToNode = Graph->FindBaseNode(Link.InputNode);

		if (!FromNode.IsValid())
		{
			MonolithDataflow::AddValidationIssue(
				Issues,
				TEXT("missing_output_node"),
				FString::Printf(TEXT("Connection references missing output node %s"), *Link.OutputNode.ToString()),
				Link.OutputNode.ToString());
		}
		if (!ToNode.IsValid())
		{
			MonolithDataflow::AddValidationIssue(
				Issues,
				TEXT("missing_input_node"),
				FString::Printf(TEXT("Connection references missing input node %s"), *Link.InputNode.ToString()),
				Link.InputNode.ToString());
		}
		if (FromNode.IsValid() && !FromNode->FindOutput(Link.Output))
		{
			MonolithDataflow::AddValidationIssue(
				Issues,
				TEXT("missing_output_pin"),
				FString::Printf(TEXT("Connection references missing output pin %s on node %s"), *Link.Output.ToString(), *FromNode->GetName().ToString()),
				Link.Output.ToString());
		}
		if (ToNode.IsValid() && !ToNode->FindInput(Link.Input))
		{
			MonolithDataflow::AddValidationIssue(
				Issues,
				TEXT("missing_input_pin"),
				FString::Printf(TEXT("Connection references missing input pin %s on node %s"), *Link.Input.ToString(), *ToNode->GetName().ToString()),
				Link.Input.ToString());
		}
	}

	TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
	Result->SetStringField(TEXT("namespace"), TEXT("dataflow"));
	Result->SetStringField(TEXT("domain"), TEXT("dataflow_graph"));
	Result->SetStringField(TEXT("asset_path"), RequestedPath);
	Result->SetStringField(TEXT("object_path"), ObjectPath);
	Result->SetBoolField(TEXT("read_only"), true);
	Result->SetNumberField(TEXT("node_count"), Graph->GetNodes().Num());
	Result->SetNumberField(TEXT("connection_count"), Graph->GetConnections().Num());
	Result->SetBoolField(TEXT("valid"), Issues.Num() == 0);
	Result->SetNumberField(TEXT("issue_count"), Issues.Num());
	Result->SetArrayField(TEXT("issues"), Issues);
	return FMonolithActionResult::Success(Result);
}

FMonolithActionResult FMonolithDataflowActions::ListDataflowVariables(const TSharedPtr<FJsonObject>& Params)
{
	FString RequestedPath;
	FString ObjectPath;
	FMonolithActionResult Error;
	UDataflow* Dataflow = MonolithDataflow::LoadDataflowAsset(Params, RequestedPath, ObjectPath, Error);
	if (!Dataflow)
	{
		return Error;
	}

	TArray<TSharedPtr<FJsonValue>> VariableRows;
	if (const UPropertyBag* PropertyBag = Dataflow->Variables.GetPropertyBagStruct())
	{
		const TConstArrayView<FPropertyBagPropertyDesc> PropertyDescs = PropertyBag->GetPropertyDescs();
		VariableRows.Reserve(PropertyDescs.Num());
		for (const FPropertyBagPropertyDesc& Desc : PropertyDescs)
		{
			VariableRows.Add(MakeShared<FJsonValueObject>(MonolithDataflow::MakeDataflowVariableJson(Dataflow, Desc)));
		}
	}

	TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
	Result->SetStringField(TEXT("namespace"), TEXT("dataflow"));
	Result->SetStringField(TEXT("domain"), TEXT("dataflow_graph"));
	Result->SetStringField(TEXT("asset_path"), RequestedPath);
	Result->SetStringField(TEXT("object_path"), ObjectPath);
	Result->SetBoolField(TEXT("read_only"), true);
	Result->SetNumberField(TEXT("variable_count"), VariableRows.Num());
	Result->SetArrayField(TEXT("variables"), VariableRows);
	return FMonolithActionResult::Success(Result);
}

FMonolithActionResult FMonolithDataflowActions::ListDataflowComments(const TSharedPtr<FJsonObject>& Params)
{
	FString RequestedPath;
	FString ObjectPath;
	FMonolithActionResult Error;
	UDataflow* Dataflow = MonolithDataflow::LoadDataflowAsset(Params, RequestedPath, ObjectPath, Error);
	if (!Dataflow)
	{
		return Error;
	}

	const int32 NodeLimit = MonolithDataflow::GetClampedIntParam(Params, TEXT("node_limit"), 128, 1, 500);
	TArray<const UEdGraphNode*> GraphNodes;
	GraphNodes.Reserve(Dataflow->Nodes.Num());
	int32 CommentCount = 0;
	for (const UEdGraphNode* Node : Dataflow->Nodes)
	{
		if (!Node)
		{
			continue;
		}

		if (Cast<UEdGraphNode_Comment>(Node))
		{
			CommentCount++;
			continue;
		}

		GraphNodes.Add(Node);
	}

	const int32 GraphNodeCount = GraphNodes.Num();
	TArray<TSharedPtr<FJsonValue>> CommentRows;
	CommentRows.Reserve(CommentCount);
	for (const UEdGraphNode* Node : Dataflow->Nodes)
	{
		if (const UEdGraphNode_Comment* Comment = Cast<UEdGraphNode_Comment>(Node))
		{
			CommentRows.Add(MakeShared<FJsonValueObject>(MonolithDataflow::MakeCommentJson(Comment, GraphNodes, NodeLimit)));
		}
	}

	TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
	Result->SetStringField(TEXT("namespace"), TEXT("dataflow"));
	Result->SetStringField(TEXT("domain"), TEXT("dataflow_graph"));
	Result->SetStringField(TEXT("asset_path"), RequestedPath);
	Result->SetStringField(TEXT("object_path"), ObjectPath);
	Result->SetBoolField(TEXT("read_only"), true);
	Result->SetNumberField(TEXT("node_limit"), NodeLimit);
	Result->SetNumberField(TEXT("graph_node_count"), GraphNodeCount);
	Result->SetNumberField(TEXT("considered_node_count"), GraphNodes.Num());
	Result->SetBoolField(TEXT("nodes_truncated"), false);
	Result->SetNumberField(TEXT("comment_count"), CommentCount);
	Result->SetNumberField(TEXT("returned_comment_count"), CommentRows.Num());
	Result->SetArrayField(TEXT("comments"), CommentRows);
	return FMonolithActionResult::Success(Result);
}
#endif
