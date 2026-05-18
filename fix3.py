import re

with open("Source/MonolithBlueprint/Private/MonolithBlueprintActions.cpp", "r") as f:
    code = f.read()

# 13. FuncsArr inputs/outputs/locals reserve
code = re.sub(
    r"(\t\t\tTArray<TSharedPtr<FJsonValue>> InputsArr;)\n(\t\t\tfor \(const UEdGraphPin\* Pin : EntryNode->Pins\))",
    r"\1\n\t\t\tInputsArr.Reserve(EntryNode->Pins.Num());\n\2",
    code,
    count=1
)

code = re.sub(
    r"(\t\tTArray<TSharedPtr<FJsonValue>> OutputsArr;)\n(\t\tif \(ResultNode\)\n\t\t{\n\t\t\tfor \(const UEdGraphPin\* Pin : ResultNode->Pins\))",
    r"\1\n\t\tif (ResultNode)\n\t\t{\n\t\t\tOutputsArr.Reserve(ResultNode->Pins.Num());\n\t\t\tfor (const UEdGraphPin* Pin : ResultNode->Pins)",
    code,
    count=1
)

code = re.sub(
    r"(\t\t\tTArray<TSharedPtr<FJsonValue>> LocalsArr;)\n(\t\t\tfor \(const FBPVariableDescription& LVar : EntryNode->LocalVariables\))",
    r"\1\n\t\t\tLocalsArr.Reserve(EntryNode->LocalVariables.Num());\n\2",
    code,
    count=1
)

code = re.sub(
    r"(\t\tTArray<TSharedPtr<FJsonValue>> PinsArr;)\n(\t\tfor \(UEdGraphNode\* Node : Graph->Nodes\))",
    r"\1\n\t\tPinsArr.Reserve(Graph->Nodes.Num());\n\2",
    code,
    count=1
)


with open("Source/MonolithBlueprint/Private/MonolithBlueprintActions.cpp", "w") as f:
    f.write(code)
