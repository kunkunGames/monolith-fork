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
## 2026-05-19 - Harden `HandleSetMontageBlend` parameter parsing
**Reliability issue:** `HandleSetMontageBlend` was using unsafe `HasField` followed by `GetNumberField` and `GetBoolField` casts, which could crash if the client provided a string or object.
**Learning:** Checking `HasField` is insufficient for type safety because it only verifies presence, not type.
**Prevention:** Replace all `HasField` and blind getter patterns with `TryGetNumberField` and `TryGetBoolField` returning an error for invalid input.
**Avoid:** Trusting `HasField` to guarantee numeric or boolean types during JSON parameter extraction.
## 2026-05-19 - Replace remaining HasField accesses in HandleCreateMontageFromSections
**Reliability issue:**  used unsafe  followed by  for optional parameters (start_time, blend_in_time, duration, etc).
**Learning:** Checking  is insufficient to guarantee type safety for JSON parameters.
**Prevention:** Replaced these specific  and blind getter patterns with  that returns a structured error for invalid types.
**Avoid:** Trusting  for optional parameters within complex action arrays.
## 2026-05-19 - Replace remaining HasField accesses in HandleCreateMontageFromSections
**Reliability issue:** `HandleCreateMontageFromSections` used unsafe `HasField` followed by `GetNumberField` for optional parameters (start_time, blend_in_time, duration, etc).
**Learning:** Checking `HasField` is insufficient to guarantee type safety for JSON parameters.
**Prevention:** Replaced these specific `HasField` and blind getter patterns with `TryGetNumberField` that returns a structured error for invalid types.
**Avoid:** Trusting `HasField` for optional parameters within complex action arrays.
## 2026-05-19 - Replace remaining HasField accesses in HandleCreateMontageFromSections
**Reliability issue:** `HandleCreateMontageFromSections` used unsafe `HasField` followed by `GetNumberField` for optional notify parameters (`time`, etc).
**Learning:** Checking `HasField` is insufficient to guarantee type safety for JSON parameters.
**Prevention:** Replaced these specific `HasField` and blind getter patterns with `TryGetNumberField` that returns a structured error for invalid types.
**Avoid:** Trusting `HasField` for optional parameters within complex action arrays.
## 2026-06-11 - Forbid template-echo PR titles and numeric branch evasion
**Coordination issue:** AnimWeaver generated branches with large numeric suffixes (e.g., `-17448107936242337015`) and used generic "concise animation-domain improvement." PR titles (e.g. ce05a1f) despite rules forbidding this.
**Learning:** General instructions in `AGENTS.md` to avoid generic placeholder names and random suffixes are often missed by agents unless directly included in their specific `.jules/<agent>.md` instructions. When an agent creates a PR title of "concise animation-domain improvement.", it is echoing the prompt's instructions rather than describing the actual change.
**Prevention:** Always replace the PR title placeholder with a concrete description of the change. Never append numeric task IDs or UUIDs to branch names to evade collision checks. If your chosen branch name is taken, stop without PR instead of renaming it.
**Avoid:** Using `AnimWeaver: concise animation-domain improvement.` as a PR title or generating branches with `-<number>` suffixes.
## 2026-07-25 - Stop emitting template-echo titles like "concise animation-domain improvement"
**Coordination issue:** AnimWeaver continued to submit PRs using the generic template-echo title "AnimWeaver: concise animation-domain improvement." (e.g. PR #1912) despite the previous rule.
**Learning:** The previous instruction to "replace the PR title placeholder" was not explicit enough to prevent the exact literal string "concise animation-domain improvement." from being used as a fallback. Agents need an explicit rejection of that specific string.
**Prevention:** You must explicitly check your proposed PR title before submission. If your title contains the literal string "concise animation-domain improvement", you MUST change it to describe the actual work (e.g., `AnimWeaver: harden blendspace array inputs`).
**Avoid:** Using the exact phrase "concise animation-domain improvement" anywhere in your PR title.
