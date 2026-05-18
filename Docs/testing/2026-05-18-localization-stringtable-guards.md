# Localization StringTable Guard Follow-up

Date: 2026-05-18

Scope: `localization.import_string_table_csv` and `localization.export_string_table_csv`.

Validation:
- Added a guard that rejects `replace_existing=true` when CSV parsing accepts zero rows, preventing accidental StringTable clears.
- Added automation coverage for the destructive empty-replace case.
- `export_string_table_csv` now reports actual file size after writing; dry-run keeps a UTF-8 byte estimate.
- Project file path checks compare path prefixes case-insensitively for Windows drive-letter/path casing.
