## 2026-05-03 - [Docs Version Sync]
**Release risk:** Stale version numbers and out-of-sync capability counts (e.g. action counts in API_REFERENCE.md and README.md vs live specs) degrade confidence and confuse agents attempting to use discovery.
**Learning:** Hard-coded versions in multiple files (`API_REFERENCE.md`, `SPEC_CORE.md`, all module specs) and action counts across docs require manual sweeping when versions bump.
**Prevention:** Always grep for `\*\*Version:\*\*` and specific action count integers in markdown docs as a post-release check.
**Avoid:** Avoid leaving README.md counts (like UI and Editor) decoupled from the detailed counts in the API reference and specs.
