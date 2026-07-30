#include "MonolithDataflowActions.h"

#include "MonolithDataflowCommon.h"

#include "Dataflow/DataflowEdNode.h"
#include "Dataflow/DataflowGraph.h"
#include "Dataflow/DataflowInputOutput.h"
#include "Dataflow/DataflowNode.h"
#include "Dataflow/DataflowNodeFactory.h"
#include "Dataflow/DataflowObject.h"
#include "EdGraphNode_Comment.h"
#include "Internationalization/Text.h"
#include "StructUtils/PropertyBag.h"
#include "UObject/UnrealType.h"

namespace MonolithDataflow
{
	namespace
	{
		inline constexpr int32 MaxContainerDepth = 16;

		template <typename EnumType>
		FString EnumToString(EnumType Value)
		{
			if (const UEnum* Enum = StaticEnum<EnumType>())
			{
				return Enum->GetNameStringByValue(static_cast<int64>(Value));
			}
			return FString::FromInt(static_cast<int32>(Value));
		}

		FMonolithActionResult ExactLoadError(const FExactDataflowLoad& Load)
		{
			const FString ErrorCode =
				Load.ErrorCode.IsEmpty()
					? TEXT("dataflow_load_failed")
					: Load.ErrorCode;
			const FString Detail =
				Load.ErrorDetail.IsEmpty()
					? FString::Printf(
						TEXT("Could not load Dataflow asset '%s'"),
						*Load.RequestedPath)
					: Load.ErrorDetail;
			const bool bInvalidPathParam =
				ErrorCode == TEXT("empty_object_path")
				|| ErrorCode == TEXT("object_path_too_long")
				|| ErrorCode == TEXT("object_path_whitespace")
				|| ErrorCode == TEXT("object_path_noncanonical")
				|| ErrorCode == TEXT("invalid_object_path")
				|| ErrorCode == TEXT("object_path_outside_game");

			TSharedPtr<FJsonObject> ErrorData = MakeShared<FJsonObject>();
			ErrorData->SetStringField(TEXT("error"), ErrorCode);
			ErrorData->SetStringField(TEXT("detail"), Detail);
			if (!Load.RequestedPath.IsEmpty())
			{
				ErrorData->SetStringField(TEXT("asset_path"), Load.RequestedPath);
			}
			ErrorData->SetBoolField(
				TEXT("package_captured_after_load"),
				Load.Package != nullptr);
			if (Load.Package)
			{
				const bool bPackageDirtyAfter = Load.Package->IsDirty();
				ErrorData->SetBoolField(
					TEXT("package_loaded_before"),
					Load.bPackageLoadedBefore);
				ErrorData->SetBoolField(
					TEXT("package_dirty_before"),
					Load.bPackageDirtyBefore);
				ErrorData->SetBoolField(
					TEXT("package_dirty_after"),
					bPackageDirtyAfter);
				ErrorData->SetBoolField(
					TEXT("package_dirty_state_preserved"),
					bPackageDirtyAfter == Load.bPackageDirtyBefore);
			}

			return FMonolithActionResult::Error(
				Detail,
				bInvalidPathParam ? ErrInvalidParams : -32603)
				.WithErrorData(ErrorData);
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
			const bool bFinite =
				FMath::IsFinite(Color.R)
				&& FMath::IsFinite(Color.G)
				&& FMath::IsFinite(Color.B)
				&& FMath::IsFinite(Color.A);
			ColorJson->SetBoolField(TEXT("finite"), bFinite);
			if (bFinite)
			{
				ColorJson->SetNumberField(TEXT("r"), Color.R);
				ColorJson->SetNumberField(TEXT("g"), Color.G);
				ColorJson->SetNumberField(TEXT("b"), Color.B);
				ColorJson->SetNumberField(TEXT("a"), Color.A);
			}
			return ColorJson;
		}

		struct FBoundedRows
		{
			TArray<TSharedPtr<FJsonValue>> Rows;
			int32 TotalCount = 0;
		};

		struct FBoundedPropertyValue
		{
			FString Value;
			FString Status = TEXT("unavailable");
			bool bAvailable = false;
			bool bTruncated = false;
		};

		FBoundedPropertyValue ReadBoundedPropertyValue(
			const FProperty* Property,
			const void* ValuePtr,
			FOutputBudget& TextBudget)
		{
			FBoundedPropertyValue Result;
			if (!Property)
			{
				Result.Status = TEXT("property_unavailable");
				return Result;
			}
			if (!ValuePtr)
			{
				Result.Status = TEXT("value_unavailable");
				return Result;
			}
			if (Property->ArrayDim != 1)
			{
				Result.Status = TEXT("omitted_fixed_array");
				return Result;
			}

			FString RawValue;
			if (const FBoolProperty* BoolProperty =
				CastField<FBoolProperty>(Property))
			{
				RawValue =
					BoolProperty->GetPropertyValue(ValuePtr)
						? TEXT("true")
						: TEXT("false");
			}
			else if (const FEnumProperty* EnumProperty =
				CastField<FEnumProperty>(Property))
			{
				const int64 RawEnumValue =
					EnumProperty->GetUnderlyingProperty()
						->GetSignedIntPropertyValue(ValuePtr);
				RawValue = EnumProperty->GetEnum()
					? EnumProperty->GetEnum()->GetNameStringByValue(RawEnumValue)
					: FString();
				if (RawValue.IsEmpty())
				{
					RawValue = FString::Printf(TEXT("%lld"), RawEnumValue);
				}
			}
			else if (const FNumericProperty* NumericProperty =
				CastField<FNumericProperty>(Property))
			{
				if (const UEnum* Enum = NumericProperty->GetIntPropertyEnum())
				{
					const int64 RawEnumValue =
						static_cast<int64>(
							NumericProperty->GetUnsignedIntPropertyValue(ValuePtr));
					RawValue = Enum->GetNameStringByValue(RawEnumValue);
					if (RawValue.IsEmpty())
					{
						RawValue = FString::Printf(TEXT("%lld"), RawEnumValue);
					}
				}
				else
				{
					RawValue =
						NumericProperty->GetNumericPropertyValueToString(ValuePtr);
				}
			}
			else if (const FNameProperty* NameProperty =
				CastField<FNameProperty>(Property))
			{
				RawValue = NameProperty->GetPropertyValue(ValuePtr).ToString();
			}
			else if (const FStrProperty* StringProperty =
				CastField<FStrProperty>(Property))
			{
				const FString& StringValue =
					StringProperty->GetPropertyValue(ValuePtr);
				Result.Value = TextBudget.Bound(StringValue, MaxTextChars);
				Result.bTruncated = Result.Value.Len() != StringValue.Len();
				Result.Status = TEXT("ok");
				Result.bAvailable = true;
				return Result;
			}
			else if (const FTextProperty* TextProperty =
				CastField<FTextProperty>(Property))
			{
				const FText& TextValue = TextProperty->GetPropertyValue(ValuePtr);
				const FString& DisplayString =
					FTextInspector::GetDisplayString(TextValue);
				Result.Value = TextBudget.Bound(DisplayString, MaxTextChars);
				Result.bTruncated = Result.Value.Len() != DisplayString.Len();
				Result.Status = TEXT("ok");
				Result.bAvailable = true;
				return Result;
			}
			else if (const FSoftObjectProperty* SoftObjectProperty =
				CastField<FSoftObjectProperty>(Property))
			{
				RawValue = SoftObjectProperty
					->GetPropertyValue(ValuePtr)
					.ToSoftObjectPath()
					.ToString();
			}
			else if (const FObjectPropertyBase* ObjectProperty =
				CastField<FObjectPropertyBase>(Property))
			{
				const UObject* Object =
					ObjectProperty->GetObjectPropertyValue(ValuePtr);
				RawValue = Object ? Object->GetPathName() : TEXT("None");
			}
			else if (CastField<FArrayProperty>(Property)
				|| CastField<FSetProperty>(Property)
				|| CastField<FMapProperty>(Property))
			{
				Result.Status = TEXT("omitted_container");
				return Result;
			}
			else if (CastField<FStructProperty>(Property))
			{
				Result.Status = TEXT("omitted_struct");
				return Result;
			}
			else
			{
				Result.Status = TEXT("omitted_unsupported_type");
				return Result;
			}

			Result.Value = TextBudget.Bound(RawValue, MaxTextChars);
			Result.bTruncated = Result.Value.Len() != RawValue.Len();
			Result.Status = TEXT("ok");
			Result.bAvailable = true;
			return Result;
		}

		TSharedPtr<FJsonObject> MakeRegisteredPinJson(
			const FDataflowConnection* Pin,
			FOutputBudget& TextBudget)
		{
			TSharedPtr<FJsonObject> PinJson = MakeShared<FJsonObject>();
			PinJson->SetStringField(TEXT("source"), TEXT("registered"));
			if (!Pin)
			{
				PinJson->SetBoolField(TEXT("valid"), false);
				PinJson->SetStringField(TEXT("error"), TEXT("null_registered_pin"));
				PinJson->SetBoolField(TEXT("guid_available"), false);
				PinJson->SetBoolField(TEXT("connected_available"), false);
				return PinJson;
			}

			PinJson->SetBoolField(TEXT("valid"), true);
			PinJson->SetStringField(
				TEXT("name"),
				TextBudget.Bound(Pin->GetName().ToString(), MaxNameChars));
			PinJson->SetStringField(
				TEXT("type"),
				TextBudget.Bound(Pin->GetType().ToString(), MaxNameChars));
			PinJson->SetStringField(TEXT("guid"), Pin->GetGuid().ToString());
			PinJson->SetBoolField(TEXT("guid_available"), true);
			PinJson->SetBoolField(TEXT("connected"), Pin->IsConnected());
			PinJson->SetBoolField(TEXT("connected_available"), true);
			PinJson->SetBoolField(TEXT("hidden"), Pin->GetPinIsHidden());
			return PinJson;
		}

		TSharedPtr<FJsonObject> MakeDeclaredPinJson(
			const UE::Dataflow::FPin& Pin,
			FOutputBudget& TextBudget)
		{
			TSharedPtr<FJsonObject> PinJson = MakeShared<FJsonObject>();
			PinJson->SetStringField(TEXT("source"), TEXT("declared"));
			PinJson->SetBoolField(TEXT("valid"), true);
			PinJson->SetStringField(
				TEXT("name"),
				TextBudget.Bound(Pin.Name.ToString(), MaxNameChars));
			PinJson->SetStringField(
				TEXT("type"),
				TextBudget.Bound(Pin.Type.ToString(), MaxNameChars));
			PinJson->SetBoolField(TEXT("guid_available"), false);
			PinJson->SetBoolField(TEXT("connected_available"), false);
			PinJson->SetBoolField(TEXT("hidden"), Pin.bHidden);
			return PinJson;
		}

		FBoundedRows MakeInputPins(
			const FDataflowNode* Node,
			int32 Limit,
			FString& OutSource,
			FOutputBudget& TextBudget)
		{
			FBoundedRows Result;
			OutSource = TEXT("unavailable");
			if (!Node)
			{
				return Result;
			}

			const TArray<FDataflowInput*> Inputs = Node->GetInputs();
			if (Inputs.Num() > 0)
			{
				OutSource = TEXT("registered");
				Result.TotalCount = Inputs.Num();
				Result.Rows.Reserve(FMath::Min(Result.TotalCount, Limit));
				for (int32 Index = 0; Index < Inputs.Num() && Index < Limit; ++Index)
				{
					if (TextBudget.TryReserveRow())
					{
						Result.Rows.Add(MakeShared<FJsonValueObject>(
							MakeRegisteredPinJson(Inputs[Index], TextBudget)));
					}
				}
				return Result;
			}

			OutSource = TEXT("declared");
			for (const UE::Dataflow::FPin& Pin : Node->GetPins())
			{
				if (Pin.Direction != UE::Dataflow::FPin::EDirection::INPUT)
				{
					continue;
				}

				++Result.TotalCount;
				if (Result.Rows.Num() < Limit
					&& TextBudget.TryReserveRow())
				{
					Result.Rows.Add(MakeShared<FJsonValueObject>(
						MakeDeclaredPinJson(Pin, TextBudget)));
				}
			}
			return Result;
		}

		FBoundedRows MakeOutputPins(
			const FDataflowNode* Node,
			int32 Limit,
			FString& OutSource,
			FOutputBudget& TextBudget)
		{
			FBoundedRows Result;
			OutSource = TEXT("unavailable");
			if (!Node)
			{
				return Result;
			}

			const TArray<FDataflowOutput*> Outputs = Node->GetOutputs();
			if (Outputs.Num() > 0)
			{
				OutSource = TEXT("registered");
				Result.TotalCount = Outputs.Num();
				Result.Rows.Reserve(FMath::Min(Result.TotalCount, Limit));
				for (int32 Index = 0; Index < Outputs.Num() && Index < Limit; ++Index)
				{
					if (TextBudget.TryReserveRow())
					{
						Result.Rows.Add(MakeShared<FJsonValueObject>(
							MakeRegisteredPinJson(Outputs[Index], TextBudget)));
					}
				}
				return Result;
			}

			OutSource = TEXT("declared");
			for (const UE::Dataflow::FPin& Pin : Node->GetPins())
			{
				if (Pin.Direction != UE::Dataflow::FPin::EDirection::OUTPUT)
				{
					continue;
				}

				++Result.TotalCount;
				if (Result.Rows.Num() < Limit
					&& TextBudget.TryReserveRow())
				{
					Result.Rows.Add(MakeShared<FJsonValueObject>(
						MakeDeclaredPinJson(Pin, TextBudget)));
				}
			}
			return Result;
		}

		FBoundedRows MakeEditableProperties(
			const FDataflowNode* Node,
			int32 Limit,
			FOutputBudget& TextBudget)
		{
			FBoundedRows Result;
			if (!Node)
			{
				return Result;
			}

			const UScriptStruct* NodeStruct = Node->TypedScriptStruct();
			if (!NodeStruct)
			{
				return Result;
			}

			for (TFieldIterator<FProperty> PropIt(
					NodeStruct,
					EFieldIteratorFlags::IncludeSuper);
				PropIt;
				++PropIt)
			{
				const FProperty* Property = *PropIt;
				if (!Property
					|| !Property->HasAnyPropertyFlags(CPF_Edit)
					|| Property->HasAnyPropertyFlags(CPF_Deprecated | CPF_Transient)
					|| Property->GetOwnerStruct() == FDataflowNode::StaticStruct())
				{
					continue;
				}

				++Result.TotalCount;
				if (Result.Rows.Num() >= Limit
					|| !TextBudget.TryReserveRow())
				{
					continue;
				}

				const void* ValuePtr = Property->ContainerPtrToValuePtr<const void>(Node);
				const FBoundedPropertyValue PropertyValue =
					ReadBoundedPropertyValue(Property, ValuePtr, TextBudget);

				TSharedPtr<FJsonObject> PropertyJson = MakeShared<FJsonObject>();
				PropertyJson->SetStringField(
					TEXT("name"),
					TextBudget.Bound(Property->GetName(), MaxNameChars));
				PropertyJson->SetStringField(
					TEXT("type"),
					TextBudget.Bound(Property->GetCPPType(), MaxNameChars));
				PropertyJson->SetStringField(
					TEXT("value_read_status"),
					PropertyValue.Status);
				PropertyJson->SetBoolField(
					TEXT("value_available"),
					PropertyValue.bAvailable);
				PropertyJson->SetBoolField(
					TEXT("value_truncated"),
					PropertyValue.bTruncated);
				if (PropertyValue.bAvailable)
				{
					PropertyJson->SetStringField(
						TEXT("value"),
						PropertyValue.Value);
				}
				PropertyJson->SetBoolField(
					TEXT("read_only"),
					Property->HasAnyPropertyFlags(CPF_EditConst));
				Result.Rows.Add(MakeShared<FJsonValueObject>(PropertyJson));
			}
			return Result;
		}

		void SetBoundedRows(
			const TSharedPtr<FJsonObject>& Object,
			const TCHAR* Prefix,
			const TCHAR* RowsField,
			const FBoundedRows& BoundedRows,
			int32 Limit)
		{
			const FString PrefixString(Prefix);
			Object->SetNumberField(PrefixString + TEXT("_count"), BoundedRows.TotalCount);
			Object->SetNumberField(
				PrefixString + TEXT("_returned_count"),
				BoundedRows.Rows.Num());
			Object->SetNumberField(PrefixString + TEXT("_limit"), Limit);
			Object->SetBoolField(
				PrefixString + TEXT("_truncated"),
				BoundedRows.TotalCount > BoundedRows.Rows.Num());
			Object->SetArrayField(RowsField, BoundedRows.Rows);
		}

		TSharedPtr<FJsonObject> MakeNodeJson(
			const UDataflow* Dataflow,
			const FDataflowNode* Node,
			int32 PinLimit,
			bool bIncludeProperties,
			int32 PropertyLimit,
			FOutputBudget& TextBudget)
		{
			TSharedPtr<FJsonObject> NodeJson = MakeShared<FJsonObject>();
			if (!Node)
			{
				NodeJson->SetBoolField(TEXT("valid"), false);
				NodeJson->SetStringField(TEXT("error"), TEXT("null_graph_node"));
				return NodeJson;
			}

			NodeJson->SetBoolField(TEXT("valid"), true);
			NodeJson->SetStringField(
				TEXT("name"),
				TextBudget.Bound(Node->GetName().ToString(), MaxNameChars));
			NodeJson->SetStringField(TEXT("guid"), Node->GetGuid().ToString());
			NodeJson->SetStringField(
				TEXT("type"),
				TextBudget.Bound(Node->GetType().ToString(), MaxNameChars));
			NodeJson->SetStringField(
				TEXT("display_name"),
				TextBudget.Bound(Node->GetDisplayName().ToString(), MaxNameChars));
			NodeJson->SetStringField(
				TEXT("category"),
				TextBudget.Bound(Node->GetCategory().ToString(), MaxNameChars));
			NodeJson->SetStringField(
				TEXT("tags"),
				TextBudget.Bound(Node->GetTags(), MaxTextChars));
			NodeJson->SetStringField(
				TEXT("tooltip"),
				TextBudget.Bound(Node->GetToolTip(), MaxTextChars));

			bool bPositionAvailable = false;
			if (Dataflow)
			{
				const TObjectPtr<const UDataflowEdNode> EdNode =
					Dataflow->FindEdNodeByDataflowNodeGuid(Node->GetGuid());
				if (EdNode)
				{
					NodeJson->SetObjectField(
						TEXT("position"),
						MakePositionJson(EdNode->NodePosX, EdNode->NodePosY));
					bPositionAvailable = true;
				}
			}
			NodeJson->SetBoolField(TEXT("position_available"), bPositionAvailable);

			FString InputSource;
			const FBoundedRows InputPins =
				MakeInputPins(Node, PinLimit, InputSource, TextBudget);
			NodeJson->SetStringField(TEXT("input_pin_source"), InputSource);
			SetBoundedRows(NodeJson, TEXT("input_pin"), TEXT("input_pins"), InputPins, PinLimit);

			FString OutputSource;
			const FBoundedRows OutputPins =
				MakeOutputPins(Node, PinLimit, OutputSource, TextBudget);
			NodeJson->SetStringField(TEXT("output_pin_source"), OutputSource);
			SetBoundedRows(NodeJson, TEXT("output_pin"), TEXT("output_pins"), OutputPins, PinLimit);

			NodeJson->SetBoolField(TEXT("properties_included"), bIncludeProperties);
			if (bIncludeProperties)
			{
				const FBoundedRows Properties =
					MakeEditableProperties(Node, PropertyLimit, TextBudget);
				SetBoundedRows(
					NodeJson,
					TEXT("property"),
					TEXT("properties"),
					Properties,
					PropertyLimit);
			}
			return NodeJson;
		}

		TSharedPtr<FJsonObject> MakeConnectionJson(
			const UE::Dataflow::FGraph& Graph,
			const UE::Dataflow::FLink& Link,
			FOutputBudget& TextBudget)
		{
			TSharedPtr<FJsonObject> LinkJson = MakeShared<FJsonObject>();
			LinkJson->SetStringField(TEXT("from_node_guid"), Link.OutputNode.ToString());
			LinkJson->SetStringField(TEXT("from_pin_guid"), Link.Output.ToString());
			LinkJson->SetStringField(TEXT("to_node_guid"), Link.InputNode.ToString());
			LinkJson->SetStringField(TEXT("to_pin_guid"), Link.Input.ToString());

			const TSharedPtr<const FDataflowNode> FromNode =
				Graph.FindBaseNode(Link.OutputNode);
			LinkJson->SetBoolField(TEXT("from_node_resolved"), FromNode.IsValid());
			if (FromNode.IsValid())
			{
				LinkJson->SetStringField(
					TEXT("from_node"),
					TextBudget.Bound(FromNode->GetName().ToString(), MaxNameChars));
				const FDataflowOutput* OutputPin = FromNode->FindOutput(Link.Output);
				LinkJson->SetBoolField(TEXT("from_pin_resolved"), OutputPin != nullptr);
				if (OutputPin)
				{
					LinkJson->SetStringField(
						TEXT("from_pin"),
						TextBudget.Bound(OutputPin->GetName().ToString(), MaxNameChars));
					LinkJson->SetStringField(
						TEXT("from_pin_type"),
						TextBudget.Bound(OutputPin->GetType().ToString(), MaxNameChars));
				}
			}
			else
			{
				LinkJson->SetBoolField(TEXT("from_pin_resolved"), false);
			}

			const TSharedPtr<const FDataflowNode> ToNode =
				Graph.FindBaseNode(Link.InputNode);
			LinkJson->SetBoolField(TEXT("to_node_resolved"), ToNode.IsValid());
			if (ToNode.IsValid())
			{
				LinkJson->SetStringField(
					TEXT("to_node"),
					TextBudget.Bound(ToNode->GetName().ToString(), MaxNameChars));
				const FDataflowInput* InputPin = ToNode->FindInput(Link.Input);
				LinkJson->SetBoolField(TEXT("to_pin_resolved"), InputPin != nullptr);
				if (InputPin)
				{
					LinkJson->SetStringField(
						TEXT("to_pin"),
						TextBudget.Bound(InputPin->GetName().ToString(), MaxNameChars));
					LinkJson->SetStringField(
						TEXT("to_pin_type"),
						TextBudget.Bound(InputPin->GetType().ToString(), MaxNameChars));
				}
			}
			else
			{
				LinkJson->SetBoolField(TEXT("to_pin_resolved"), false);
			}
			return LinkJson;
		}

		TSharedPtr<FJsonObject> MakeFactoryParamsJson(
			const UE::Dataflow::FFactoryParameters& FactoryParams,
			bool bIncludePins,
			int32 PinLimit,
			bool bIncludeProperties,
			int32 PropertyLimit,
			FOutputBudget& TextBudget)
		{
			TSharedPtr<FJsonObject> NodeType = MakeShared<FJsonObject>();
			NodeType->SetStringField(
				TEXT("type_name"),
				TextBudget.Bound(FactoryParams.TypeName.ToString(), MaxNameChars));
			NodeType->SetStringField(
				TEXT("display_name"),
				TextBudget.Bound(FactoryParams.DisplayName.ToString(), MaxNameChars));
			NodeType->SetStringField(
				TEXT("category"),
				TextBudget.Bound(FactoryParams.Category.ToString(), MaxNameChars));
			NodeType->SetStringField(
				TEXT("tags"),
				TextBudget.Bound(FactoryParams.Tags, MaxTextChars));
			NodeType->SetStringField(
				TEXT("tooltip"),
				TextBudget.Bound(FactoryParams.ToolTip, MaxTextChars));
			NodeType->SetStringField(
				TEXT("version"),
				TextBudget.Bound(FactoryParams.GetVersion().ToString(), MaxNameChars));
			NodeType->SetBoolField(TEXT("deprecated"), FactoryParams.IsDeprecated());
			NodeType->SetBoolField(TEXT("experimental"), FactoryParams.IsExperimental());

			const FDataflowNode* DefaultNode =
				FactoryParams.DefaultNodeObject.IsValid()
					? FactoryParams.DefaultNodeObject.Get()
					: nullptr;
			NodeType->SetBoolField(TEXT("default_node_available"), DefaultNode != nullptr);
			NodeType->SetBoolField(TEXT("pins_included"), bIncludePins);
			if (bIncludePins)
			{
				FString InputSource;
				const FBoundedRows InputPins =
					MakeInputPins(DefaultNode, PinLimit, InputSource, TextBudget);
				NodeType->SetStringField(TEXT("input_pin_source"), InputSource);
				SetBoundedRows(
					NodeType,
					TEXT("input_pin"),
					TEXT("input_pins"),
					InputPins,
					PinLimit);

				FString OutputSource;
				const FBoundedRows OutputPins =
					MakeOutputPins(DefaultNode, PinLimit, OutputSource, TextBudget);
				NodeType->SetStringField(TEXT("output_pin_source"), OutputSource);
				SetBoundedRows(
					NodeType,
					TEXT("output_pin"),
					TEXT("output_pins"),
					OutputPins,
					PinLimit);
			}

			NodeType->SetBoolField(TEXT("properties_included"), bIncludeProperties);
			if (bIncludeProperties)
			{
				const FBoundedRows Properties =
					MakeEditableProperties(DefaultNode, PropertyLimit, TextBudget);
				SetBoundedRows(
					NodeType,
					TEXT("property"),
					TEXT("properties"),
					Properties,
					PropertyLimit);
			}
			return NodeType;
		}

		struct FIssueAccumulator
		{
			explicit FIssueAccumulator(int32 InLimit, FOutputBudget& InTextBudget)
				: Limit(InLimit)
				, TextBudget(InTextBudget)
			{
				Rows.Reserve(InLimit);
			}

			void Add(
				const FString& Code,
				const FString& Detail,
				const FString& Guid = FString())
			{
				++TotalCount;
				if (Rows.Num() >= Limit
					|| !TextBudget.TryReserveRow())
				{
					return;
				}

				TSharedPtr<FJsonObject> Issue = MakeShared<FJsonObject>();
				Issue->SetStringField(
					TEXT("code"),
					TextBudget.Bound(Code, MaxNameChars));
				Issue->SetStringField(
					TEXT("detail"),
					TextBudget.Bound(Detail, MaxTextChars));
				if (!Guid.IsEmpty())
				{
					Issue->SetStringField(TEXT("guid"), Guid);
				}
				Rows.Add(MakeShared<FJsonValueObject>(Issue));
			}

			TArray<TSharedPtr<FJsonValue>> Rows;
			int32 TotalCount = 0;
			int32 Limit = 0;
			FOutputBudget& TextBudget;
		};

		TArray<TSharedPtr<FJsonValue>> MakeContainerTypeList(
			const FPropertyBagContainerTypes& ContainerTypes,
			FOutputBudget& TextBudget,
			bool& bOutTruncated)
		{
			TArray<TSharedPtr<FJsonValue>> Rows;
			const int32 ReturnedCount =
				FMath::Min(static_cast<int32>(ContainerTypes.Num()), MaxContainerDepth);
			Rows.Reserve(ReturnedCount);
			int32 Index = 0;
			for (const EPropertyBagContainerType ContainerType : ContainerTypes)
			{
				if (Index++ >= MaxContainerDepth)
				{
					break;
				}
				if (!TextBudget.TryReserveRow())
				{
					continue;
				}
				Rows.Add(MakeShared<FJsonValueString>(
					TextBudget.Bound(EnumToString(ContainerType), MaxNameChars)));
			}
			bOutTruncated = static_cast<int32>(ContainerTypes.Num()) > Rows.Num();
			return Rows;
		}

		TSharedPtr<FJsonObject> MakeDataflowVariableJson(
			const UDataflow* Dataflow,
			const FPropertyBagPropertyDesc& Desc,
			FOutputBudget& TextBudget)
		{
			TSharedPtr<FJsonObject> Variable = MakeShared<FJsonObject>();
			Variable->SetStringField(
				TEXT("name"),
				TextBudget.Bound(Desc.Name.ToString(), MaxNameChars));
			Variable->SetStringField(TEXT("guid"), Desc.ID.ToString());
			Variable->SetStringField(
				TEXT("value_type"),
				TextBudget.Bound(EnumToString(Desc.ValueType), MaxNameChars));
			Variable->SetNumberField(
				TEXT("container_depth"),
				static_cast<int32>(Desc.ContainerTypes.Num()));
			Variable->SetBoolField(
				TEXT("is_container"),
				!Desc.ContainerTypes.IsEmpty());
			Variable->SetStringField(
				TEXT("container_type"),
				Desc.ContainerTypes.IsEmpty()
					? TEXT("None")
					: TextBudget.Bound(
						EnumToString(Desc.ContainerTypes.GetFirstContainerType()),
						MaxNameChars));
			bool bContainerTypesTruncated = false;
			const TArray<TSharedPtr<FJsonValue>> ContainerTypes =
				MakeContainerTypeList(
					Desc.ContainerTypes,
					TextBudget,
					bContainerTypesTruncated);
			Variable->SetNumberField(
				TEXT("returned_container_type_count"),
				ContainerTypes.Num());
			Variable->SetBoolField(
				TEXT("container_types_truncated"),
				bContainerTypesTruncated);
			Variable->SetArrayField(TEXT("container_types"), ContainerTypes);
			Variable->SetBoolField(
				TEXT("read_only"),
				(Desc.PropertyFlags & CPF_EditConst) != 0);

			if (Desc.ValueTypeObject)
			{
				Variable->SetStringField(
					TEXT("value_type_object"),
					TextBudget.Bound(Desc.ValueTypeObject->GetName(), MaxNameChars));
				Variable->SetStringField(
					TEXT("value_type_object_path"),
					TextBudget.Bound(Desc.ValueTypeObject->GetPathName(), MaxPathChars));
				Variable->SetStringField(
					TEXT("value_type_object_class"),
					TextBudget.Bound(
						Desc.ValueTypeObject->GetClass()->GetName(),
						MaxNameChars));
			}

			if (!Desc.ContainerTypes.IsEmpty())
			{
				Variable->SetStringField(
					TEXT("value_read_status"),
					TEXT("omitted_container"));
				Variable->SetBoolField(TEXT("value_available"), false);
				Variable->SetBoolField(TEXT("value_truncated"), false);
			}
			else
			{
				const FConstStructView ValueView = Dataflow->Variables.GetValue();
				const void* ValuePtr =
					ValueView.IsValid() && Desc.CachedProperty
						? Desc.CachedProperty->ContainerPtrToValuePtr<const void>(
							ValueView.GetMemory())
						: nullptr;
				const FBoundedPropertyValue PropertyValue =
					ReadBoundedPropertyValue(
						Desc.CachedProperty,
						ValuePtr,
						TextBudget);
				Variable->SetStringField(
					TEXT("value_read_status"),
					PropertyValue.Status);
				Variable->SetBoolField(
					TEXT("value_available"),
					PropertyValue.bAvailable);
				Variable->SetBoolField(
					TEXT("value_truncated"),
					PropertyValue.bTruncated);
				if (PropertyValue.bAvailable)
				{
					Variable->SetStringField(
						TEXT("value"),
						PropertyValue.Value);
				}
			}
			return Variable;
		}

		bool IsNodeInsideComment(
			const UEdGraphNode_Comment* Comment,
			const UEdGraphNode* Node)
		{
			if (!Comment || !Node || Node == Comment)
			{
				return false;
			}

			const double CommentRight =
				static_cast<double>(Comment->NodePosX)
				+ static_cast<double>(Comment->NodeWidth);
			const double CommentBottom =
				static_cast<double>(Comment->NodePosY)
				+ static_cast<double>(Comment->NodeHeight);
			return static_cast<double>(Node->NodePosX) >= Comment->NodePosX
				&& static_cast<double>(Node->NodePosY) >= Comment->NodePosY
				&& static_cast<double>(Node->NodePosX) <= CommentRight
				&& static_cast<double>(Node->NodePosY) <= CommentBottom;
		}

		TSharedPtr<FJsonObject> MakeContainedEdNodeJson(
			const UEdGraphNode* Node,
			FOutputBudget& TextBudget)
		{
			TSharedPtr<FJsonObject> NodeJson = MakeShared<FJsonObject>();
			if (!Node)
			{
				NodeJson->SetBoolField(TEXT("valid"), false);
				NodeJson->SetStringField(TEXT("error"), TEXT("null_editor_node"));
				return NodeJson;
			}

			NodeJson->SetBoolField(TEXT("valid"), true);
			NodeJson->SetStringField(TEXT("guid"), Node->NodeGuid.ToString());
			NodeJson->SetStringField(
				TEXT("name"),
				TextBudget.Bound(Node->GetName(), MaxNameChars));
			NodeJson->SetStringField(
				TEXT("class"),
				TextBudget.Bound(Node->GetClass()->GetName(), MaxNameChars));
			NodeJson->SetObjectField(
				TEXT("position"),
				MakePositionJson(Node->NodePosX, Node->NodePosY));

			if (const UDataflowEdNode* DataflowEdNode = Cast<UDataflowEdNode>(Node))
			{
				NodeJson->SetStringField(
					TEXT("dataflow_node_guid"),
					DataflowEdNode->GetDataflowNodeGuid().ToString());
				const TSharedPtr<const FDataflowNode> DataflowNode =
					DataflowEdNode->GetDataflowNode();
				NodeJson->SetBoolField(
					TEXT("dataflow_node_resolved"),
					DataflowNode.IsValid());
				if (DataflowNode.IsValid())
				{
					NodeJson->SetStringField(
						TEXT("dataflow_node_name"),
						TextBudget.Bound(
							DataflowNode->GetName().ToString(),
							MaxNameChars));
					NodeJson->SetStringField(
						TEXT("dataflow_node_type"),
						TextBudget.Bound(
							DataflowNode->GetType().ToString(),
							MaxNameChars));
				}
			}
			return NodeJson;
		}

		TSharedPtr<FJsonObject> MakeCommentJson(
			const UEdGraphNode_Comment* Comment,
			const TArray<const UEdGraphNode*>& ConsideredGraphNodes,
			int32 NodeLimit,
			bool bGraphScanComplete,
			FOutputBudget& TextBudget)
		{
			TSharedPtr<FJsonObject> CommentJson = MakeShared<FJsonObject>();
			if (!Comment)
			{
				CommentJson->SetBoolField(TEXT("valid"), false);
				CommentJson->SetStringField(TEXT("error"), TEXT("null_comment"));
				return CommentJson;
			}

			TArray<TSharedPtr<FJsonValue>> ContainedNodes;
			ContainedNodes.Reserve(
				FMath::Min(ConsideredGraphNodes.Num(), NodeLimit));
			int32 ObservedContainedCount = 0;
			for (const UEdGraphNode* Node : ConsideredGraphNodes)
			{
				if (!IsNodeInsideComment(Comment, Node))
				{
					continue;
				}

				++ObservedContainedCount;
				if (ContainedNodes.Num() < NodeLimit
					&& TextBudget.TryReserveRow())
				{
					ContainedNodes.Add(MakeShared<FJsonValueObject>(
						MakeContainedEdNodeJson(Node, TextBudget)));
				}
			}

			CommentJson->SetBoolField(TEXT("valid"), true);
			CommentJson->SetStringField(TEXT("guid"), Comment->NodeGuid.ToString());
			CommentJson->SetStringField(
				TEXT("comment"),
				TextBudget.Bound(Comment->NodeComment, MaxTextChars));
			CommentJson->SetStringField(
				TEXT("details"),
				TextBudget.Bound(Comment->NodeDetails.ToString(), MaxTextChars));
			CommentJson->SetStringField(
				TEXT("class"),
				TextBudget.Bound(Comment->GetClass()->GetName(), MaxNameChars));
			CommentJson->SetObjectField(
				TEXT("position"),
				MakePositionJson(Comment->NodePosX, Comment->NodePosY));
			CommentJson->SetObjectField(
				TEXT("size"),
				MakeSizeJson(Comment->NodeWidth, Comment->NodeHeight));
			CommentJson->SetObjectField(
				TEXT("color"),
				MakeColorJson(Comment->CommentColor));
			CommentJson->SetNumberField(TEXT("font_size"), Comment->FontSize);
			CommentJson->SetStringField(
				TEXT("move_mode"),
				Comment->MoveMode == ECommentBoxMode::GroupMovement
					? TEXT("GroupMovement")
					: TEXT("NoGroupMovement"));
			CommentJson->SetNumberField(TEXT("comment_depth"), Comment->CommentDepth);
			CommentJson->SetNumberField(
				TEXT("observed_contained_node_count"),
				ObservedContainedCount);
			CommentJson->SetNumberField(
				TEXT("returned_contained_node_count"),
				ContainedNodes.Num());
			CommentJson->SetNumberField(TEXT("node_limit"), NodeLimit);
			CommentJson->SetBoolField(
				TEXT("contained_node_count_complete"),
				bGraphScanComplete);
			if (bGraphScanComplete)
			{
				CommentJson->SetNumberField(
					TEXT("contained_node_count"),
					ObservedContainedCount);
			}
			CommentJson->SetBoolField(
				TEXT("contained_nodes_truncated"),
				ObservedContainedCount > ContainedNodes.Num());
			CommentJson->SetBoolField(
				TEXT("contained_nodes_complete"),
				bGraphScanComplete
					&& ObservedContainedCount == ContainedNodes.Num());
			CommentJson->SetArrayField(TEXT("contained_nodes"), ContainedNodes);
			return CommentJson;
		}
	}
}

FMonolithActionResult FMonolithDataflowActions::GetDataflowGraph(
	const TSharedPtr<FJsonObject>& Params)
{
	using namespace MonolithDataflow;

	FString AssetPath;
	int32 NodeLimit = 128;
	int32 ConnectionLimit = 1000;
	int32 PinLimit = 128;
	int32 PropertyLimit = 128;
	bool bIncludeProperties = false;
	FStrictParamReader Reader(Params);
	if (!Reader.RequiredString(TEXT("asset_path"), AssetPath, MaxPathChars)
		|| !Reader.OptionalInt(
			TEXT("node_limit"),
			NodeLimit,
			128,
			1,
			MaxGraphNodes)
		|| !Reader.OptionalInt(
			TEXT("connection_limit"),
			ConnectionLimit,
			1000,
			1,
			MaxGraphConnections)
		|| !Reader.OptionalInt(
			TEXT("pin_limit"),
			PinLimit,
			128,
			1,
			MaxPinsPerOwner)
		|| !Reader.OptionalInt(
			TEXT("property_limit"),
			PropertyLimit,
			128,
			1,
			MaxPropertiesPerOwner)
		|| !Reader.OptionalBool(
			TEXT("include_properties"),
			bIncludeProperties,
			false)
		|| !Reader.RejectUnknown(
			{
				TEXT("asset_path"),
				TEXT("node_limit"),
				TEXT("connection_limit"),
				TEXT("pin_limit"),
				TEXT("property_limit"),
				TEXT("include_properties")
			}))
	{
		return InvalidParams(Reader.GetError());
	}

	const FExactDataflowLoad Load = LoadExactDataflowAsset(AssetPath);
	if (!Load.IsExact())
	{
		return ExactLoadError(Load);
	}

	const auto Graph = Load.Asset->GetDataflow();
	if (!Graph.IsValid())
	{
		return ErrorWithCode(
			TEXT("missing_dataflow_graph"),
			FString::Printf(
				TEXT("Dataflow asset '%s' has no graph"),
				*AssetPath),
			AssetPath);
	}

	FOutputBudget TextBudget;
	const TArray<TSharedPtr<FDataflowNode>>& Nodes = Graph->GetNodes();
	TArray<TSharedPtr<FJsonValue>> NodeRows;
	NodeRows.Reserve(FMath::Min(Nodes.Num(), NodeLimit));
	for (int32 Index = 0; Index < Nodes.Num() && Index < NodeLimit; ++Index)
	{
		if (!TextBudget.TryReserveRow())
		{
			break;
		}
		NodeRows.Add(MakeShared<FJsonValueObject>(MakeNodeJson(
			Load.Asset,
			Nodes[Index].Get(),
			PinLimit,
			bIncludeProperties,
			PropertyLimit,
			TextBudget)));
	}

	const TArray<UE::Dataflow::FLink>& Connections = Graph->GetConnections();
	TArray<TSharedPtr<FJsonValue>> ConnectionRows;
	ConnectionRows.Reserve(FMath::Min(Connections.Num(), ConnectionLimit));
	for (int32 Index = 0;
		Index < Connections.Num() && Index < ConnectionLimit;
		++Index)
	{
		if (!TextBudget.TryReserveRow())
		{
			break;
		}
		ConnectionRows.Add(MakeShared<FJsonValueObject>(
			MakeConnectionJson(*Graph, Connections[Index], TextBudget)));
	}

	TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
	Result->SetStringField(TEXT("namespace"), TEXT("dataflow"));
	Result->SetStringField(TEXT("domain"), TEXT("dataflow_graph"));
	Result->SetStringField(TEXT("asset_path"), AssetPath);
	Result->SetStringField(TEXT("object_path"), Load.ResolvedPath);
	Result->SetBoolField(TEXT("read_only"), true);
	Result->SetStringField(
		TEXT("dataflow_type"),
		EnumToString(Load.Asset->Type));
	Result->SetNumberField(TEXT("node_count"), Nodes.Num());
	Result->SetNumberField(TEXT("returned_node_count"), NodeRows.Num());
	Result->SetNumberField(TEXT("node_limit"), NodeLimit);
	Result->SetBoolField(TEXT("nodes_truncated"), Nodes.Num() > NodeRows.Num());
	Result->SetNumberField(TEXT("connection_count"), Connections.Num());
	Result->SetNumberField(
		TEXT("returned_connection_count"),
		ConnectionRows.Num());
	Result->SetNumberField(TEXT("connection_limit"), ConnectionLimit);
	Result->SetBoolField(
		TEXT("connections_truncated"),
		Connections.Num() > ConnectionRows.Num());
	Result->SetBoolField(
		TEXT("truncated"),
		Nodes.Num() > NodeRows.Num()
			|| Connections.Num() > ConnectionRows.Num()
			|| TextBudget.IsExhausted());
	Result->SetBoolField(
		TEXT("connection_slice_independent_of_node_slice"),
		true);
	Result->SetNumberField(TEXT("pin_limit_per_direction"), PinLimit);
	Result->SetBoolField(TEXT("properties_included"), bIncludeProperties);
	Result->SetNumberField(TEXT("property_limit_per_node"), PropertyLimit);
	AddOutputBudgetFields(Result, TextBudget);
	Result->SetArrayField(TEXT("nodes"), NodeRows);
	Result->SetArrayField(TEXT("connections"), ConnectionRows);
	return FinalizeReadOnlyResult(Load, Result);
}

FMonolithActionResult FMonolithDataflowActions::ListDataflowNodeTypes(
	const TSharedPtr<FJsonObject>& Params)
{
	using namespace MonolithDataflow;

	FString Filter;
	bool bCommonOnly = true;
	bool bIncludePins = false;
	int32 Limit = 200;
	int32 PinLimit = 64;
	FStrictParamReader Reader(Params);
	if (!Reader.OptionalString(TEXT("filter"), Filter, FString(), MaxTextChars)
		|| !Reader.OptionalBool(TEXT("common_only"), bCommonOnly, true)
		|| !Reader.OptionalInt(
			TEXT("limit"),
			Limit,
			200,
			1,
			MaxNodeTypes)
		|| !Reader.OptionalBool(TEXT("include_pins"), bIncludePins, false)
		|| !Reader.OptionalInt(
			TEXT("pin_limit"),
			PinLimit,
			64,
			1,
			MaxPinsPerOwner)
		|| !Reader.RejectUnknown(
			{
				TEXT("filter"),
				TEXT("common_only"),
				TEXT("limit"),
				TEXT("include_pins"),
				TEXT("pin_limit")
			}))
	{
		return InvalidParams(Reader.GetError());
	}

	UE::Dataflow::FNodeFactory* Factory =
		UE::Dataflow::FNodeFactory::GetInstance();
	if (!Factory)
	{
		return ErrorWithCode(
			TEXT("node_factory_unavailable"),
			TEXT("Dataflow node factory is not available"));
	}

	TArray<UE::Dataflow::FFactoryParameters> RegisteredParams =
		Factory->RegisteredParameters();
	RegisteredParams.Sort(
		[](const UE::Dataflow::FFactoryParameters& A,
			const UE::Dataflow::FFactoryParameters& B)
		{
			const int32 CategoryCompare =
				A.Category.ToString().Compare(
					B.Category.ToString(),
					ESearchCase::CaseSensitive);
			return CategoryCompare == 0
				? A.TypeName.ToString().Compare(
					B.TypeName.ToString(),
					ESearchCase::CaseSensitive) < 0
				: CategoryCompare < 0;
		});

	FOutputBudget TextBudget;
	TArray<TSharedPtr<FJsonValue>> Rows;
	Rows.Reserve(FMath::Min(RegisteredParams.Num(), Limit));
	int32 ValidRegisteredCount = 0;
	int32 MatchedCount = 0;
	for (const UE::Dataflow::FFactoryParameters& FactoryParams : RegisteredParams)
	{
		if (!FactoryParams.IsValid())
		{
			continue;
		}
		++ValidRegisteredCount;

		if (bCommonOnly
			&& (FactoryParams.IsDeprecated() || FactoryParams.IsExperimental()))
		{
			continue;
		}
		if (!Filter.IsEmpty())
		{
			const FString Haystack = FString::Printf(
				TEXT("%s\n%s\n%s\n%s"),
				*FactoryParams.TypeName.ToString(),
				*FactoryParams.DisplayName.ToString(),
				*FactoryParams.Category.ToString(),
				*FactoryParams.Tags);
			if (!Haystack.Contains(Filter, ESearchCase::IgnoreCase))
			{
				continue;
			}
		}

		++MatchedCount;
		if (Rows.Num() < Limit
			&& TextBudget.TryReserveRow())
		{
			Rows.Add(MakeShared<FJsonValueObject>(MakeFactoryParamsJson(
				FactoryParams,
				bIncludePins,
				PinLimit,
				false,
				0,
				TextBudget)));
		}
	}

	TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
	Result->SetStringField(TEXT("namespace"), TEXT("dataflow"));
	Result->SetStringField(TEXT("domain"), TEXT("dataflow_node_types"));
	Result->SetStringField(TEXT("ordering"), TEXT("category_then_type_case_exact"));
	Result->SetStringField(TEXT("filter"), Filter);
	Result->SetStringField(
		TEXT("filter_semantics"),
		TEXT("case_insensitive_substring"));
	Result->SetBoolField(TEXT("common_only"), bCommonOnly);
	Result->SetBoolField(TEXT("include_pins"), bIncludePins);
	Result->SetNumberField(TEXT("pin_limit_per_direction"), PinLimit);
	Result->SetNumberField(
		TEXT("registered_entry_count"),
		RegisteredParams.Num());
	Result->SetNumberField(
		TEXT("valid_registered_entry_count"),
		ValidRegisteredCount);
	Result->SetNumberField(TEXT("matched_count"), MatchedCount);
	Result->SetNumberField(TEXT("returned_count"), Rows.Num());
	Result->SetNumberField(TEXT("limit"), Limit);
	Result->SetBoolField(
		TEXT("truncated"),
		MatchedCount > Rows.Num() || TextBudget.IsExhausted());
	AddOutputBudgetFields(Result, TextBudget);
	Result->SetArrayField(TEXT("node_types"), Rows);
	return FMonolithActionResult::Success(Result);
}

FMonolithActionResult FMonolithDataflowActions::GetDataflowNodeSchema(
	const TSharedPtr<FJsonObject>& Params)
{
	using namespace MonolithDataflow;

	FString TypeName;
	bool bIncludeProperties = true;
	int32 PinLimit = 256;
	int32 PropertyLimit = 256;
	FStrictParamReader Reader(Params);
	if (!Reader.RequiredString(TEXT("type_name"), TypeName, MaxNameChars)
		|| !Reader.OptionalBool(
			TEXT("include_properties"),
			bIncludeProperties,
			true)
		|| !Reader.OptionalInt(
			TEXT("pin_limit"),
			PinLimit,
			256,
			1,
			MaxPinsPerOwner)
		|| !Reader.OptionalInt(
			TEXT("property_limit"),
			PropertyLimit,
			256,
			1,
			MaxPropertiesPerOwner)
		|| !Reader.RejectUnknown(
			{
				TEXT("type_name"),
				TEXT("include_properties"),
				TEXT("pin_limit"),
				TEXT("property_limit")
			}))
	{
		return InvalidParams(Reader.GetError());
	}

	UE::Dataflow::FNodeFactory* Factory =
		UE::Dataflow::FNodeFactory::GetInstance();
	if (!Factory)
	{
		return ErrorWithCode(
			TEXT("node_factory_unavailable"),
			TEXT("Dataflow node factory is not available"));
	}

	TArray<UE::Dataflow::FFactoryParameters> RegisteredParams =
		Factory->RegisteredParameters();
	const UE::Dataflow::FFactoryParameters* ExactMatch = nullptr;
	FString CaseInsensitiveMatch;
	for (const UE::Dataflow::FFactoryParameters& FactoryParams : RegisteredParams)
	{
		const FString RegisteredTypeName = FactoryParams.TypeName.ToString();
		if (RegisteredTypeName.Equals(TypeName, ESearchCase::CaseSensitive))
		{
			ExactMatch = &FactoryParams;
			break;
		}
		if (CaseInsensitiveMatch.IsEmpty()
			&& RegisteredTypeName.Equals(TypeName, ESearchCase::IgnoreCase))
		{
			CaseInsensitiveMatch = RegisteredTypeName;
		}
	}

	if (!ExactMatch)
	{
		if (!CaseInsensitiveMatch.IsEmpty())
		{
			return ErrorWithCode(
				TEXT("node_type_case_mismatch"),
				FString::Printf(
					TEXT("Requested node type '%s' differs in case from registered type '%s'"),
					*TypeName,
					*CaseInsensitiveMatch));
		}
		return ErrorWithCode(
			TEXT("unknown_node_type"),
			FString::Printf(
				TEXT("No case-exact registered Dataflow node type named '%s'"),
				*TypeName));
	}
	if (!ExactMatch->IsValid())
	{
		return ErrorWithCode(
			TEXT("invalid_registered_node_type"),
			FString::Printf(
				TEXT("Registered Dataflow node type '%s' has no valid factory parameters"),
				*TypeName));
	}

	FOutputBudget TextBudget;
	TSharedPtr<FJsonObject> Result = MakeFactoryParamsJson(
		*ExactMatch,
		true,
		PinLimit,
		bIncludeProperties,
		PropertyLimit,
		TextBudget);
	Result->SetStringField(TEXT("namespace"), TEXT("dataflow"));
	Result->SetStringField(TEXT("domain"), TEXT("dataflow_node_schema"));
	Result->SetStringField(TEXT("requested_type_name"), TypeName);
	Result->SetBoolField(TEXT("type_name_case_exact"), true);
	AddOutputBudgetFields(Result, TextBudget);
	return FMonolithActionResult::Success(Result);
}

FMonolithActionResult FMonolithDataflowActions::ValidateDataflowGraph(
	const TSharedPtr<FJsonObject>& Params)
{
	using namespace MonolithDataflow;

	FString AssetPath;
	int32 NodeScanLimit = 10000;
	int32 ConnectionScanLimit = 50000;
	int32 IssueLimit = 500;
	FStrictParamReader Reader(Params);
	if (!Reader.RequiredString(TEXT("asset_path"), AssetPath, MaxPathChars)
		|| !Reader.OptionalInt(
			TEXT("node_scan_limit"),
			NodeScanLimit,
			10000,
			1,
			MaxNodeScan)
		|| !Reader.OptionalInt(
			TEXT("connection_scan_limit"),
			ConnectionScanLimit,
			50000,
			1,
			MaxConnectionScan)
		|| !Reader.OptionalInt(
			TEXT("issue_limit"),
			IssueLimit,
			500,
			1,
			MaxValidationIssues)
		|| !Reader.RejectUnknown(
			{
				TEXT("asset_path"),
				TEXT("node_scan_limit"),
				TEXT("connection_scan_limit"),
				TEXT("issue_limit")
			}))
	{
		return InvalidParams(Reader.GetError());
	}

	const FExactDataflowLoad Load = LoadExactDataflowAsset(AssetPath);
	if (!Load.IsExact())
	{
		return ExactLoadError(Load);
	}

	const auto Graph = Load.Asset->GetDataflow();
	if (!Graph.IsValid())
	{
		return ErrorWithCode(
			TEXT("missing_dataflow_graph"),
			FString::Printf(
				TEXT("Dataflow asset '%s' has no graph"),
				*AssetPath),
			AssetPath);
	}

	const TArray<TSharedPtr<FDataflowNode>>& Nodes = Graph->GetNodes();
	const TArray<UE::Dataflow::FLink>& Connections = Graph->GetConnections();
	const int32 ScannedNodeCount = FMath::Min(Nodes.Num(), NodeScanLimit);
	const int32 ScannedConnectionCount =
		FMath::Min(Connections.Num(), ConnectionScanLimit);
	const bool bNodeScanComplete = ScannedNodeCount == Nodes.Num();
	const bool bConnectionScanComplete =
		ScannedConnectionCount == Connections.Num();
	const bool bValidationComplete =
		bNodeScanComplete && bConnectionScanComplete;

	FOutputBudget TextBudget;
	FIssueAccumulator Issues(IssueLimit, TextBudget);
	TSet<FName> NodeNames;
	TSet<FGuid> NodeGuids;
	NodeNames.Reserve(ScannedNodeCount);
	NodeGuids.Reserve(ScannedNodeCount);

	for (int32 Index = 0; Index < ScannedNodeCount; ++Index)
	{
		const TSharedPtr<FDataflowNode>& Node = Nodes[Index];
		if (!Node.IsValid())
		{
			Issues.Add(
				TEXT("null_graph_node"),
				FString::Printf(TEXT("Graph node entry %d is null"), Index));
			continue;
		}

		const FName NodeName = Node->GetName();
		const FGuid NodeGuid = Node->GetGuid();
		if (NodeName.IsNone())
		{
			Issues.Add(
				TEXT("invalid_node_name"),
				FString::Printf(
					TEXT("Graph node entry %d has no name"),
					Index),
				NodeGuid.ToString());
		}
		else if (NodeNames.Contains(NodeName))
		{
			Issues.Add(
				TEXT("duplicate_node_name"),
				FString::Printf(
					TEXT("Duplicate Dataflow node name '%s'"),
					*NodeName.ToString()),
				NodeGuid.ToString());
		}
		NodeNames.Add(NodeName);

		if (!NodeGuid.IsValid())
		{
			Issues.Add(
				TEXT("invalid_node_guid"),
				FString::Printf(
					TEXT("Graph node entry %d has an invalid GUID"),
					Index));
		}
		else if (NodeGuids.Contains(NodeGuid))
		{
			Issues.Add(
				TEXT("duplicate_node_guid"),
				FString::Printf(
					TEXT("Duplicate Dataflow node GUID '%s'"),
					*NodeGuid.ToString()),
				NodeGuid.ToString());
		}
		NodeGuids.Add(NodeGuid);
	}

	for (int32 Index = 0; Index < ScannedConnectionCount; ++Index)
	{
		const UE::Dataflow::FLink& Link = Connections[Index];
		if (!Link.OutputNode.IsValid())
		{
			Issues.Add(
				TEXT("invalid_output_node_guid"),
				FString::Printf(
					TEXT("Connection entry %d has an invalid output-node GUID"),
					Index));
		}
		if (!Link.InputNode.IsValid())
		{
			Issues.Add(
				TEXT("invalid_input_node_guid"),
				FString::Printf(
					TEXT("Connection entry %d has an invalid input-node GUID"),
					Index));
		}

		const TSharedPtr<const FDataflowNode> FromNode =
			Graph->FindBaseNode(Link.OutputNode);
		const TSharedPtr<const FDataflowNode> ToNode =
			Graph->FindBaseNode(Link.InputNode);
		if (!FromNode.IsValid())
		{
			Issues.Add(
				TEXT("missing_output_node"),
				FString::Printf(
					TEXT("Connection entry %d references missing output node '%s'"),
					Index,
					*Link.OutputNode.ToString()),
				Link.OutputNode.ToString());
		}
		if (!ToNode.IsValid())
		{
			Issues.Add(
				TEXT("missing_input_node"),
				FString::Printf(
					TEXT("Connection entry %d references missing input node '%s'"),
					Index,
					*Link.InputNode.ToString()),
				Link.InputNode.ToString());
		}

		const FDataflowOutput* OutputPin =
			FromNode.IsValid() ? FromNode->FindOutput(Link.Output) : nullptr;
		const FDataflowInput* InputPin =
			ToNode.IsValid() ? ToNode->FindInput(Link.Input) : nullptr;
		if (FromNode.IsValid() && !OutputPin)
		{
			Issues.Add(
				TEXT("missing_output_pin"),
				FString::Printf(
					TEXT("Connection entry %d references missing output pin '%s' on node '%s'"),
					Index,
					*Link.Output.ToString(),
					*FromNode->GetName().ToString()),
				Link.Output.ToString());
		}
		if (ToNode.IsValid() && !InputPin)
		{
			Issues.Add(
				TEXT("missing_input_pin"),
				FString::Printf(
					TEXT("Connection entry %d references missing input pin '%s' on node '%s'"),
					Index,
					*Link.Input.ToString(),
					*ToNode->GetName().ToString()),
				Link.Input.ToString());
		}
		if (OutputPin && InputPin
			&& !OutputPin->GetType().ToString().Equals(
				InputPin->GetType().ToString(),
				ESearchCase::CaseSensitive))
		{
			Issues.Add(
				TEXT("connection_type_mismatch"),
				FString::Printf(
					TEXT("Connection entry %d links output type '%s' to input type '%s'"),
					Index,
					*OutputPin->GetType().ToString(),
					*InputPin->GetType().ToString()));
		}
	}

	const FString ValidityStatus = !bValidationComplete
		? TEXT("incomplete")
		: Issues.TotalCount == 0
			? TEXT("valid")
			: TEXT("invalid");

	TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
	Result->SetStringField(TEXT("namespace"), TEXT("dataflow"));
	Result->SetStringField(TEXT("domain"), TEXT("dataflow_validation"));
	Result->SetStringField(TEXT("asset_path"), AssetPath);
	Result->SetStringField(TEXT("object_path"), Load.ResolvedPath);
	Result->SetBoolField(TEXT("read_only"), true);
	Result->SetNumberField(TEXT("node_count"), Nodes.Num());
	Result->SetNumberField(TEXT("scanned_node_count"), ScannedNodeCount);
	Result->SetNumberField(TEXT("node_scan_limit"), NodeScanLimit);
	Result->SetBoolField(TEXT("node_scan_complete"), bNodeScanComplete);
	Result->SetNumberField(TEXT("connection_count"), Connections.Num());
	Result->SetNumberField(
		TEXT("scanned_connection_count"),
		ScannedConnectionCount);
	Result->SetNumberField(
		TEXT("connection_scan_limit"),
		ConnectionScanLimit);
	Result->SetBoolField(
		TEXT("connection_scan_complete"),
		bConnectionScanComplete);
	Result->SetBoolField(
		TEXT("validation_complete"),
		bValidationComplete);
	Result->SetStringField(TEXT("validity_status"), ValidityStatus);
	if (bValidationComplete)
	{
		Result->SetBoolField(TEXT("valid"), Issues.TotalCount == 0);
	}
	Result->SetNumberField(TEXT("issue_count"), Issues.TotalCount);
	Result->SetNumberField(TEXT("returned_issue_count"), Issues.Rows.Num());
	Result->SetNumberField(TEXT("issue_limit"), IssueLimit);
	Result->SetBoolField(
		TEXT("issues_truncated"),
		Issues.TotalCount > Issues.Rows.Num());
	AddOutputBudgetFields(Result, TextBudget);
	Result->SetArrayField(TEXT("issues"), Issues.Rows);
	return FinalizeReadOnlyResult(Load, Result);
}

FMonolithActionResult FMonolithDataflowActions::ListDataflowVariables(
	const TSharedPtr<FJsonObject>& Params)
{
	using namespace MonolithDataflow;

	FString AssetPath;
	int32 Limit = 200;
	FStrictParamReader Reader(Params);
	if (!Reader.RequiredString(TEXT("asset_path"), AssetPath, MaxPathChars)
		|| !Reader.OptionalInt(
			TEXT("limit"),
			Limit,
			200,
			1,
			MaxVariables)
		|| !Reader.RejectUnknown({TEXT("asset_path"), TEXT("limit")}))
	{
		return InvalidParams(Reader.GetError());
	}

	const FExactDataflowLoad Load = LoadExactDataflowAsset(AssetPath);
	if (!Load.IsExact())
	{
		return ExactLoadError(Load);
	}

	FOutputBudget TextBudget;
	TArray<TSharedPtr<FJsonValue>> VariableRows;
	int32 VariableCount = 0;
	bool bPropertyBagAvailable = false;
	if (const UPropertyBag* PropertyBag =
		Load.Asset->Variables.GetPropertyBagStruct())
	{
		bPropertyBagAvailable = true;
		const TConstArrayView<FPropertyBagPropertyDesc> PropertyDescs =
			PropertyBag->GetPropertyDescs();
		VariableCount = PropertyDescs.Num();
		VariableRows.Reserve(FMath::Min(VariableCount, Limit));
		for (int32 Index = 0;
			Index < PropertyDescs.Num() && Index < Limit;
			++Index)
		{
			if (!TextBudget.TryReserveRow())
			{
				break;
			}
			VariableRows.Add(MakeShared<FJsonValueObject>(
				MakeDataflowVariableJson(
					Load.Asset,
					PropertyDescs[Index],
					TextBudget)));
		}
	}

	TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
	Result->SetStringField(TEXT("namespace"), TEXT("dataflow"));
	Result->SetStringField(TEXT("domain"), TEXT("dataflow_variables"));
	Result->SetStringField(TEXT("asset_path"), AssetPath);
	Result->SetStringField(TEXT("object_path"), Load.ResolvedPath);
	Result->SetBoolField(TEXT("read_only"), true);
	Result->SetBoolField(
		TEXT("property_bag_available"),
		bPropertyBagAvailable);
	Result->SetNumberField(TEXT("variable_count"), VariableCount);
	Result->SetNumberField(
		TEXT("returned_variable_count"),
		VariableRows.Num());
	Result->SetNumberField(TEXT("limit"), Limit);
	Result->SetBoolField(
		TEXT("variables_truncated"),
		VariableCount > VariableRows.Num());
	AddOutputBudgetFields(Result, TextBudget);
	Result->SetArrayField(TEXT("variables"), VariableRows);
	return FinalizeReadOnlyResult(Load, Result);
}

FMonolithActionResult FMonolithDataflowActions::ListDataflowComments(
	const TSharedPtr<FJsonObject>& Params)
{
	using namespace MonolithDataflow;

	FString AssetPath;
	int32 CommentLimit = 200;
	int32 NodeLimit = 128;
	int32 GraphNodeScanLimit = 5000;
	FStrictParamReader Reader(Params);
	if (!Reader.RequiredString(TEXT("asset_path"), AssetPath, MaxPathChars)
		|| !Reader.OptionalInt(
			TEXT("comment_limit"),
			CommentLimit,
			200,
			1,
			MaxComments)
		|| !Reader.OptionalInt(
			TEXT("node_limit"),
			NodeLimit,
			128,
			1,
			MaxCommentNodes)
		|| !Reader.OptionalInt(
			TEXT("graph_node_scan_limit"),
			GraphNodeScanLimit,
			5000,
			1,
			MaxCommentGraphNodeScan)
		|| !Reader.RejectUnknown(
			{
				TEXT("asset_path"),
				TEXT("comment_limit"),
				TEXT("node_limit"),
				TEXT("graph_node_scan_limit")
			}))
	{
		return InvalidParams(Reader.GetError());
	}

	const int64 RequestedComparisonBudget =
		static_cast<int64>(CommentLimit)
		* static_cast<int64>(GraphNodeScanLimit);
	if (RequestedComparisonBudget > MaxCommentMembershipChecks)
	{
		return InvalidParams(FString::Printf(
			TEXT("comment_limit * graph_node_scan_limit must not exceed %lld"),
			MaxCommentMembershipChecks));
	}

	const FExactDataflowLoad Load = LoadExactDataflowAsset(AssetPath);
	if (!Load.IsExact())
	{
		return ExactLoadError(Load);
	}

	const int32 EditorGraphEntryCount = Load.Asset->Nodes.Num();
	const int32 ScannedEditorGraphEntryCount =
		FMath::Min(EditorGraphEntryCount, GraphNodeScanLimit);
	const bool bGraphScanComplete =
		ScannedEditorGraphEntryCount == EditorGraphEntryCount;

	TArray<const UEdGraphNode*> ConsideredGraphNodes;
	ConsideredGraphNodes.Reserve(ScannedEditorGraphEntryCount);
	TArray<const UEdGraphNode_Comment*> ConsideredComments;
	ConsideredComments.Reserve(
		FMath::Min(ScannedEditorGraphEntryCount, CommentLimit));
	int32 NullEditorGraphEntryCount = 0;
	int32 ObservedCommentCount = 0;
	int32 ConsideredNonCommentNodeCount = 0;
	for (int32 Index = 0; Index < ScannedEditorGraphEntryCount; ++Index)
	{
		const UEdGraphNode* Node = Load.Asset->Nodes[Index];
		if (!Node)
		{
			++NullEditorGraphEntryCount;
			continue;
		}
		if (const UEdGraphNode_Comment* Comment =
			Cast<UEdGraphNode_Comment>(Node))
		{
			++ObservedCommentCount;
			if (ConsideredComments.Num() < CommentLimit)
			{
				ConsideredComments.Add(Comment);
			}
		}
		else
		{
			++ConsideredNonCommentNodeCount;
		}
		// Comments stay in the membership candidate list so that a comment box
		// nested inside another one is reported as a contained node.
		// IsNodeInsideComment already rejects self-membership.
		ConsideredGraphNodes.Add(Node);
	}

	FOutputBudget TextBudget;
	TArray<TSharedPtr<FJsonValue>> CommentRows;
	CommentRows.Reserve(ConsideredComments.Num());
	for (const UEdGraphNode_Comment* Comment : ConsideredComments)
	{
		if (!TextBudget.TryReserveRow())
		{
			break;
		}
		CommentRows.Add(MakeShared<FJsonValueObject>(MakeCommentJson(
			Comment,
			ConsideredGraphNodes,
			NodeLimit,
			bGraphScanComplete,
			TextBudget)));
	}

	TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
	Result->SetStringField(TEXT("namespace"), TEXT("dataflow"));
	Result->SetStringField(TEXT("domain"), TEXT("dataflow_comments"));
	Result->SetStringField(TEXT("asset_path"), AssetPath);
	Result->SetStringField(TEXT("object_path"), Load.ResolvedPath);
	Result->SetBoolField(TEXT("read_only"), true);
	Result->SetNumberField(
		TEXT("editor_graph_entry_count"),
		EditorGraphEntryCount);
	Result->SetNumberField(
		TEXT("scanned_editor_graph_entry_count"),
		ScannedEditorGraphEntryCount);
	Result->SetNumberField(
		TEXT("graph_node_scan_limit"),
		GraphNodeScanLimit);
	Result->SetBoolField(
		TEXT("graph_scan_complete"),
		bGraphScanComplete);
	Result->SetNumberField(
		TEXT("null_editor_graph_entry_count"),
		NullEditorGraphEntryCount);
	Result->SetNumberField(
		TEXT("considered_non_comment_node_count"),
		ConsideredNonCommentNodeCount);
	Result->SetNumberField(
		TEXT("considered_membership_candidate_count"),
		ConsideredGraphNodes.Num());
	Result->SetNumberField(
		TEXT("observed_comment_count"),
		ObservedCommentCount);
	Result->SetBoolField(
		TEXT("comment_count_complete"),
		bGraphScanComplete);
	if (bGraphScanComplete)
	{
		Result->SetNumberField(TEXT("comment_count"), ObservedCommentCount);
	}
	Result->SetNumberField(
		TEXT("returned_comment_count"),
		CommentRows.Num());
	Result->SetNumberField(TEXT("comment_limit"), CommentLimit);
	Result->SetNumberField(TEXT("node_limit_per_comment"), NodeLimit);
	Result->SetBoolField(
		TEXT("comments_truncated"),
		ObservedCommentCount > CommentRows.Num());
	Result->SetBoolField(
		TEXT("comments_complete"),
		bGraphScanComplete
			&& ObservedCommentCount == CommentRows.Num()
			&& !TextBudget.IsExhausted());
	Result->SetBoolField(
		TEXT("truncated"),
		!bGraphScanComplete
			|| ObservedCommentCount > CommentRows.Num()
			|| TextBudget.IsExhausted());
	Result->SetNumberField(
		TEXT("requested_membership_comparison_budget"),
		static_cast<double>(RequestedComparisonBudget));
	Result->SetNumberField(
		TEXT("maximum_membership_comparison_budget"),
		static_cast<double>(MaxCommentMembershipChecks));
	Result->SetNumberField(
		TEXT("actual_membership_comparison_upper_bound"),
		static_cast<double>(
			static_cast<int64>(ConsideredComments.Num())
			* static_cast<int64>(ConsideredGraphNodes.Num())));
	AddOutputBudgetFields(Result, TextBudget);
	Result->SetArrayField(TEXT("comments"), CommentRows);
	return FinalizeReadOnlyResult(Load, Result);
}
