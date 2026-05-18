# Niagara List Limit Guards

Date: 2026-05-18

Scope: `niagara.list_systems`, `niagara.list_module_scripts`, and `niagara.search_dynamic_inputs`.

Validation:
- Added `FMonolithNiagaraActions::ClampNiagaraQueryLimit` coverage for negative, zero, in-range, and oversized limits.
- Confirmed the public contract: absent `limit` preserves action defaults, present non-numeric `limit` returns invalid-param, and numeric limits clamp to `[1, 1000]`.
