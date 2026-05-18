import re

with open("Source/MonolithBlueprint/Private/MonolithBlueprintActions.cpp", "r") as f:
    code = f.read()

# 11. ComponentsArr.Reserve
code = re.sub(
    r"(\tTArray<TSharedPtr<FJsonValue>> ComponentsArr;\n\tint32 ComponentCount = 0;\n\n\tUSimpleConstructionScript\* SCS = BP->SimpleConstructionScript;\n\tif \(SCS\)\n\t{)",
    r"\1\n\t\tComponentsArr.Reserve(SCS->GetRootNodes().Num());",
    code,
    count=1
)

# 12. PinsArr.Reserve (NodeDetails)
code = re.sub(
    r"(\tTArray<TSharedPtr<FJsonValue>> PinsArr;)\n(\tfor \(const UEdGraphPin\* Pin : Node->Pins\))",
    r"\1\n\tPinsArr.Reserve(Node->Pins.Num());\n\2",
    code,
    count=1
)

with open("Source/MonolithBlueprint/Private/MonolithBlueprintActions.cpp", "w") as f:
    f.write(code)
