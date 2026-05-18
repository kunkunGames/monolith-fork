import re

with open("Source/MonolithBlueprint/Private/MonolithBlueprintActions.cpp", "r") as f:
    code = f.read()

# 1. GraphsArr.Reserve
code = re.sub(
    r"(\tTArray<TSharedPtr<FJsonValue>> GraphsArr;)\n(\tMonolithBlueprintInternal::AddGraphArray)",
    r"\1\n\tGraphsArr.Reserve(BP->UbergraphPages.Num() + BP->FunctionGraphs.Num() + BP->MacroGraphs.Num() + BP->DelegateSignatureGraphs.Num());\n\2",
    code,
    count=1
)

# 2. NodesArr.Reserve (graph data)
code = re.sub(
    r"(\tTArray<TSharedPtr<FJsonValue>> NodesArr;)\n(\tfor \(UEdGraphNode\* Node : Graph->Nodes\))",
    r"\1\n\tNodesArr.Reserve(Graph->Nodes.Num());\n\2",
    code,
    count=1
)

# 3. NodesArr.Reserve (graph summary 1)
code = re.sub(
    r"(\t\tTArray<TSharedPtr<FJsonValue>> NodesArr;)\n(\t\tSummarizeGraph\(Graph, NodesArr\);)",
    r"\1\n\t\tNodesArr.Reserve(Graph->Nodes.Num());\n\2",
    code,
    count=1
)

# 4. NodesArr.Reserve (graph summary 2)
code = re.sub(
    r"(\t\t\tTArray<TSharedPtr<FJsonValue>> NodesArr;)\n(\t\t\tSummarizeGraph\(Graph, NodesArr\);)",
    r"\1\n\t\t\tNodesArr.Reserve(Graph->Nodes.Num());\n\2",
    code,
    count=1
)

# 5. VarsArr.Reserve
code = re.sub(
    r"(\tTArray<TSharedPtr<FJsonValue>> VarsArr;)\n\n(\tUClass\* GenClass = BP->GeneratedClass;)",
    r"\1\n\tVarsArr.Reserve(BP->NewVariables.Num());\n\n\2",
    code,
    count=1
)

# 6. FuncsArr.Reserve
code = re.sub(
    r"(\tTArray<TSharedPtr<FJsonValue>> FuncsArr;)\n\n(\tfor \(UEdGraph\* Graph : BP->FunctionGraphs\))",
    r"\1\n\tFuncsArr.Reserve(BP->FunctionGraphs.Num());\n\n\2",
    code,
    count=1
)

# 7. DispatchersArr.Reserve
code = re.sub(
    r"(\tTArray<TSharedPtr<FJsonValue>> DispatchersArr;)\n\n(\tfor \(UEdGraph\* Graph : BP->DelegateSignatureGraphs\))",
    r"\1\n\tDispatchersArr.Reserve(BP->DelegateSignatureGraphs.Num());\n\n\2",
    code,
    count=1
)

# 8. InterfacesArr.Reserve
code = re.sub(
    r"(\tTArray<TSharedPtr<FJsonValue>> InterfacesArr;)\n\n(\tfor \(const FBPInterfaceDescription& Iface : BP->ImplementedInterfaces\))",
    r"\1\n\tInterfacesArr.Reserve(BP->ImplementedInterfaces.Num());\n\n\2",
    code,
    count=1
)

# 9. NodesArr.Reserve (construction script)
code = re.sub(
    r"(\tTArray<TSharedPtr<FJsonValue>> NodesArr;)\n(\tfor \(UEdGraphNode\* Node : CSGraph->Nodes\))",
    r"\1\n\tNodesArr.Reserve(CSGraph->Nodes.Num());\n\2",
    code,
    count=1
)

# 10. GraphNamesArr.Reserve
code = re.sub(
    r"(\tTArray<TSharedPtr<FJsonValue>> GraphNamesArr;)\n(\tfor \(const UEdGraph\* G : BP->UbergraphPages\))",
    r"\1\n\tGraphNamesArr.Reserve(BP->UbergraphPages.Num() + BP->FunctionGraphs.Num() + BP->MacroGraphs.Num());\n\2",
    code,
    count=1
)

with open("Source/MonolithBlueprint/Private/MonolithBlueprintActions.cpp", "w") as f:
    f.write(code)
