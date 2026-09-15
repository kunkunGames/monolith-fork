import pathlib

files = [
    "Source/MonolithAI/Private/MonolithAIModule.cpp",
    "Source/MonolithAI/Private/MonolithAIStateTreeActions.cpp",
    "Source/MonolithAnimation/Private/MonolithMirrorTableActions.cpp",
    "Source/MonolithAnimation/Private/MonolithPoseSearchActions.cpp",
    "Source/MonolithAnimation/Private/MonolithRetargetSettingsActions.cpp",
    "Source/MonolithAudio/Private/MonolithAudioSoundCueActions.cpp",
    "Source/MonolithBlueprint/Private/MonolithBlueprintActions.cpp",
    "Source/MonolithConfig/MonolithConfig.Build.cs",
    "Source/MonolithConfig/Private/MonolithConfigModule.cpp",
    "Source/MonolithConfig/Private/MonolithLocalizationActions.cpp",
    "Source/MonolithConfig/Public/MonolithLocalizationActions.h",
    "Source/MonolithGAS/MonolithGAS.Build.cs",
    "Source/MonolithGAS/Private/MonolithGASInputAssetActions.cpp",
    "Source/MonolithGAS/Private/MonolithGASModule.cpp",
    "Source/MonolithGAS/Public/MonolithGASInputAssetActions.h",
]

root = pathlib.Path(".")
out_dir = pathlib.Path("_conflict_dumps")
out_dir.mkdir(exist_ok=True)

for f in files:
    p = root / f
    text = p.read_text(encoding="utf-8", errors="replace")
    lines = text.splitlines()
    i = 0
    n = 0
    summary = []
    while i < len(lines):
        if lines[i].startswith("<<<<<<<"):
            n += 1
            start = i
            mid = None
            end = None
            for j in range(i, len(lines)):
                if lines[j].startswith("=======") and mid is None:
                    mid = j
                elif lines[j].startswith(">>>>>>>") and mid is not None:
                    end = j
                    break
            head = lines[start + 1 : mid]
            theirs = lines[mid + 1 : end]
            summary.append(
                f"CONFLICT {n} lines {start+1}-{end+1} HEAD={len(head)} THEIRS={len(theirs)}"
            )
            dump = out_dir / (f.replace("/", "_").replace("\\", "_") + f".c{n}.txt")
            with dump.open("w", encoding="utf-8") as fh:
                fh.write(f"FILE {f} CONFLICT {n} {start+1}-{end+1}\n")
                fh.write(f"HEAD {len(head)} lines THEIRS {len(theirs)} lines\n")
                fh.write("===== HEAD =====\n")
                for idx, line in enumerate(head):
                    fh.write(f"{start+2+idx:6d}|{line}\n")
                fh.write("===== THEIRS =====\n")
                for idx, line in enumerate(theirs):
                    fh.write(f"{mid+2+idx:6d}|{line}\n")
            i = end + 1
        else:
            i += 1
    print(f"{f}: {n} conflicts")
    for s in summary:
        print("  " + s)
