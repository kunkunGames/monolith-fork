# Monolith Hosted Static CI Catalog Bootstrap Verification

**Date:** 2026-08-02

**Scope:** Align clean GitHub checkouts with the v0.22 ignored generated-catalog contract

**Result:** Hosted CI now creates the source snapshot before benchmark and static checks; the checker enforces that workflow order

---

## 1. Contract Correction

| Before | After | Reason |
|---|---|---|
| The generated source catalog was removed from Git, but Hosted Static CI expected the file to exist immediately after checkout. | CI runs the repository-owned generator before `ci_static_checks.py`. | A clean checkout receives the same deterministic catalog input as local release/query validation without tracking generated output. |
| Workflow scope only rejected Unreal build tokens and duplicate workflow files. | `required_ordered_tokens` requires catalog generation before the static checker. | A future workflow edit cannot silently restore the missing-input race. |
| Static-checker diagnostics called the source snapshot tracked. | Diagnostics and QueryHelp describe the generated/ignored ownership contract. | Documentation and errors now match actual repository ownership. |

## 2. Verification Gates

| Gate | Required result |
|---|---|
| Catalog generate plus `--check` | Current snapshot with the exact action count produced by the source tree |
| Static checker self-test | Clean fixture plus missing/reordered prerequisite regression cases pass |
| Workflow ordering audit | Generator command precedes `ci_static_checks.py` and both exact tokens are configured |
| Git hygiene | Generated JSON remains ignored and absent from the commit |
| Hosted follow-up | New master run no longer reports the missing catalog or ActionGuidance missing-file blockers |

## 3. Boundary

This correction removes only the v0.22 clean-checkout regression. Pre-existing
benchmark-contract and project-shaped-host failures remain separate and are not
reclassified as passing.

Screenshot verification and Discord upload are not applicable because this is
a nonvisual GitHub workflow, static-checker, and documentation correction.
