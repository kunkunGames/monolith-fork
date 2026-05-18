# Animation ParamGuard

Date: 2026-05-18

Scope: `animation.set_sequence_properties`, `animation.set_additive_settings`, and `animation.set_compression_settings` parameter parsing.

Validation:
- Added an automation regression for `set_sequence_properties` rejecting present wrong-type `rate_scale`.
- The handler keeps absent-value defaults distinct from present malformed values, matching sibling ParamGuard PR behavior.
- Local static CI script passed with zero blocking findings; `.claude/agents` remains an external advisory prerequisite.
