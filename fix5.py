import re

with open("Source/MonolithBlueprint/Private/MonolithBlueprintActions.cpp", "r") as f:
    code = f.read()

# Fix 17: PropsArr (skip due to TFieldIterator and unknown count, usually loop counter is needed but hard here)
# Fix 18: InputsArr/OutputsArr (search_functions)
code = re.sub(
    r"(\t\tTArray<TSharedPtr<FJsonValue>> InputsArr;)\n(\t\tfor \(const auto& P : Entry\.Inputs\))",
    r"\1\n\t\tInputsArr.Reserve(Entry.Inputs.Num());\n\2",
    code,
    count=1
)
code = re.sub(
    r"(\t\tTArray<TSharedPtr<FJsonValue>> OutputsArr;)\n(\t\tfor \(const auto& P : Entry\.Outputs\))",
    r"\1\n\t\tOutputsArr.Reserve(Entry.Outputs.Num());\n\2",
    code,
    count=1
)

# Fix 19: ConnArr
code = re.sub(
    r"(\t\tTArray<TSharedPtr<FJsonValue>> ConnArr;)\n(\t\tfor \(const UEdGraphPin\* Linked : Pin->LinkedTo\))",
    r"\1\n\t\tConnArr.Reserve(Pin->LinkedTo.Num());\n\2",
    code,
    count=1
)

# Fix 20: PinsArr (HandleGetEventDispatcherSignature)
code = re.sub(
    r"(\tTArray<TSharedPtr<FJsonValue>> PinsArr;)\n(\tfor \(UEdGraphNode\* Node : SigGraph->Nodes\))",
    r"\1\n\tPinsArr.Reserve(SigGraph->Nodes.Num());\n\2",
    code,
    count=1
)

with open("Source/MonolithBlueprint/Private/MonolithBlueprintActions.cpp", "w") as f:
    f.write(code)
