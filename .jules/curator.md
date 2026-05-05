## 2026-05-05 - Add missing ignore rules for generated databases and release zips
**Hygiene issue:** Tools generate SQLite databases (`EngineSource.db`, `ProjectIndex.db`) and release scripts create zip packages (`Monolith-v*.zip`) that lack explicit ignores, increasing the risk of accidental commits.
**Learning:** Monolith uses standalone Python scripts and PowerShell wrappers that generate artifacts at root or subfolders which were never explicitly ignored, likely because they fall outside typical Unreal `Intermediate/` or `Saved/` patterns.
**Prevention:** Add explicit `EngineSource.db`, `ProjectIndex.db`, and `Monolith-v*.zip` entries to `.gitignore`.
**Avoid:** Assuming UE's standard `Saved/` or `Intermediate/` ignores will cover all generated database and release artifacts created by custom tooling.
