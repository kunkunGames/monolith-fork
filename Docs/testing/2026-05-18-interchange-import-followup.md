# Interchange Import Review Follow-up

Date: 2026-05-18

Scope: `interchange` import/reimport guardrails.

Validation:
- Documented `conflict_policy=rename` as best-effort pass-through; callers must inspect `imported_assets` for final package names.
- Documented `conflict_policy=fail` as conservative preflight based on the expected package derived from the source filename.
- Tightened `confirm` and `dry_run` parsing so present wrong-type values produce validation messages instead of being treated as false.
- Fixed formatting drift in the `reimport_asset` failure branch.
