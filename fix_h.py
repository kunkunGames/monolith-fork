file_path = "Source/MonolithNiagara/Public/MonolithNiagaraActions.h"
with open(file_path, "r") as f:
    text = f.read()

new_method = """	// --- Utility ---
	static int32 ClampNiagaraQueryLimit(int32 Limit, int32 Max = 1000);

	// --- Initialization ---"""

text = text.replace("	// --- Initialization ---", new_method)

with open(file_path, "w") as f:
    f.write(text)
