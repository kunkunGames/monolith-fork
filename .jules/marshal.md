
## 2024-05-24 - Standardize branch naming and verification claims
**Coordination issue:** Agents used inconsistent branch prefixes (e.g., `bolt-*`, `perf-*`, `sentinel-*`) making duplicate detection extremely difficult.
**Learning:** Without a single, strict convention, agents fail to correctly parse `git branch -r` and identify overlapping work.
**Prevention:** Created root `AGENTS.md` explicitly requiring the `jules/<agent>/<module-or-area>/<short-behavior>` pattern and banning ad-hoc prefixes. Also clarified no-op acceptability and verification claim truthfulness.
**Avoid:** Agents creating un-prefixed branches or blindly claiming Unreal Engine verification in headless VMs.

## 2024-05-25 - Harden WorkFingerprint and Duplicate check requirements
**Coordination issue:** Agents were not consistently reporting their duplicate checks, and WorkFingerprints lacked sufficient detail (like risk type and API/docs impact) to effectively deduplicate work.
**Learning:** Broad requirements for WorkFingerprints and duplicate checks lead to agents omitting crucial information needed by other agents to determine overlap.
**Prevention:** Updated `AGENTS.md` to explicitly require a 'Duplicate check' section in PRs and expanded the `WorkFingerprint` to include module, component/action/helper, risk type, public API impact, and docs/spec impact.
**Avoid:** Submitting PRs without explicit duplicate check documentation or with sparse WorkFingerprints that fail to identify potential overlaps.
