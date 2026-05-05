
## 2024-05-24 - Standardize branch naming and verification claims
**Coordination issue:** Agents used inconsistent branch prefixes (e.g., `bolt-*`, `perf-*`, `sentinel-*`) making duplicate detection extremely difficult.
**Learning:** Without a single, strict convention, agents fail to correctly parse `git branch -r` and identify overlapping work.
**Prevention:** Created root `AGENTS.md` explicitly requiring the `jules/<agent>/<module-or-area>/<short-behavior>` pattern and banning ad-hoc prefixes. Also clarified no-op acceptability and verification claim truthfulness.
**Avoid:** Agents creating un-prefixed branches or blindly claiming Unreal Engine verification in headless VMs.
