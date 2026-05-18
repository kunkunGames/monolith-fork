import re

with open("Source/MonolithBlueprint/Private/MonolithBlueprintActions.cpp", "r") as f:
    code = f.read()

# Fix 14: ExecTargets
code = re.sub(
    r"(\t\t\tTArray<TSharedPtr<FJsonValue>> ExecTargets;)\n(\t\t\tfor \(const UEdGraphPin\* Pin : Node->Pins\))",
    r"\1\n\t\t\tExecTargets.Reserve(Node->Pins.Num());\n\2",
    code,
    count=1
)

# Fix 15: ChildrenArr
code = re.sub(
    r"(\t\tTArray<TSharedPtr<FJsonValue>> ChildrenArr;)\n(\t\tfor \(USCS_Node\* Child : Node->GetChildNodes\(\)\))",
    r"\1\n\t\tChildrenArr.Reserve(Node->GetChildNodes().Num());\n\2",
    code,
    count=1
)

# Fix 16: NativeComponentsArr
code = re.sub(
    r"(\t\t\tTArray<UActorComponent\*> NativeComps;\n\t\t\tCDO->GetComponents\(NativeComps\);\n\t\t\tfor \(UActorComponent\* Comp : NativeComps\))",
    r"\t\t\tTArray<UActorComponent*> NativeComps;\n\t\t\tCDO->GetComponents(NativeComps);\n\t\t\tNativeComponentsArr.Reserve(NativeComps.Num());\n\t\t\tfor (UActorComponent* Comp : NativeComps)",
    code,
    count=1
)

with open("Source/MonolithBlueprint/Private/MonolithBlueprintActions.cpp", "w") as f:
    f.write(code)
