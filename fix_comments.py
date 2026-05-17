import re

file_path = "Source/MonolithNiagara/Public/MonolithNiagaraActions.h"
with open(file_path, "r") as f:
    text = f.read()

# remove the 3 old clamp methods
text = re.sub(r'\tstatic int32 ClampListModuleScriptsLimit\(int32 Limit\);\n\tstatic int32 ClampSearchDynamicInputsLimit\(int32 Limit\);\n\tstatic int32 ClampListSystemsLimit\(int32 Limit\);\n\n', '', text)

# add the new single clamp method at a better location
new_method = """	// --- Utility ---
	static int32 ClampNiagaraQueryLimit(int32 Limit, int32 Max = 1000);

	// --- Initialization ---"""

text = text.replace("	// --- Initialization ---", new_method)

with open(file_path, "w") as f:
    f.write(text)

file_path = "Source/MonolithNiagara/Private/MonolithNiagaraActions.cpp"
with open(file_path, "r") as f:
    text = f.read()

text = re.sub(
r'''int32 FMonolithNiagaraActions::ClampListModuleScriptsLimit\(int32 Limit\)
\{
	return FMath::Clamp\(Limit, 1, 1000\);
\}

int32 FMonolithNiagaraActions::ClampSearchDynamicInputsLimit\(int32 Limit\)
\{
	return FMath::Clamp\(Limit, 1, 1000\);
\}

int32 FMonolithNiagaraActions::ClampListSystemsLimit\(int32 Limit\)
\{
	return FMath::Clamp\(Limit, 1, 1000\);
\}''',
r'''int32 FMonolithNiagaraActions::ClampNiagaraQueryLimit(int32 Limit, int32 Max)
{
	return FMath::Clamp(Limit, 1, Max);
}''', text)

text = text.replace("Limit = ClampListModuleScriptsLimit(Limit);", "Limit = ClampNiagaraQueryLimit(Limit);")
text = text.replace("Limit = ClampSearchDynamicInputsLimit(Limit);", "Limit = ClampNiagaraQueryLimit(Limit);")
text = text.replace("Limit = ClampListSystemsLimit(Limit);", "Limit = ClampNiagaraQueryLimit(Limit);")

with open(file_path, "w") as f:
    f.write(text)

file_path = "Source/MonolithNiagara/Private/Tests/MonolithNiagaraResourceBoundaryTests.cpp"
with open(file_path, "r") as f:
    text = f.read()

text = text.replace("ClampListSystemsLimit", "ClampNiagaraQueryLimit")
text = text.replace("ClampSearchDynamicInputsLimit", "ClampNiagaraQueryLimit")
text = text.replace("ClampListModuleScriptsLimit", "ClampNiagaraQueryLimit")

with open(file_path, "w") as f:
    f.write(text)
