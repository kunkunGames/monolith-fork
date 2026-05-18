# AnimWeaver PR Guidance

## PR Intent
AnimWeaver PRs improve animation and PoseSearch action safety while preserving the authoring behavior expected by animation clients.

## Code Work Improvements
- Replace unsafe field access with `TryGet*Field`, but keep optional defaults identical unless explicitly documented.
- Verify UE 5.7 animation, PoseSearch, IKRig, and Control Rig APIs before changing signatures or includes.
- Avoid mixing param safety with new animation actions or doc count changes.

## Review Gate
- Search changed handlers for `HasField` followed by `Get*Field`.
- Compile if any animation module C++ changes; editor-only animation APIs drift frequently.

## 2026-05-14 - Rewrite broad animation type-safety PRs before merge
**Reliability issue:** A broad AnimWeaver JSON type-safety PR became mergeable after a PoseSearch overlap was removed, but the remaining AnimationActions changes still silently defaulted malformed present fields.
**Learning:** Removing the direct overlap is not enough; the remaining diff must still follow ParamGuard semantics and preserve asset mutation behavior for invalid client input.
**Prevention:** Reopen animation JSON hardening as focused action-level PRs that reject wrong-type optional fields with invalid-param errors and only default absent fields.
**Avoid:** Merging broad `TryGet*Field` refactors that touch many animation handlers, contain formatting drift, or convert wrong-type values into defaults.
