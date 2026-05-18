import re

with open("Source/MonolithBlueprint/Private/MonolithBlueprintActions.cpp", "r") as f:
    code = f.read()

# Fix 21: SearchFunctions Results.Reserve -> we have a 'Limit' so we can reserve Limit
code = re.sub(
    r"(\tTArray<TSharedPtr<FJsonValue>> Results;)\n(\tfor \(const FFunctionCacheEntry& Entry : Cache\))",
    r"\1\n\tResults.Reserve(Limit);\n\2",
    code,
    count=1
)

with open("Source/MonolithBlueprint/Private/MonolithBlueprintActions.cpp", "w") as f:
    f.write(code)
