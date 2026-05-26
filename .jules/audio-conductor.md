2026-05-14 - Malformed Input Checking
Target: Audio asset actions
Learning: TryGet*Field handles type coercion errors silently when returning false, meaning fields with incorrect types can mutate assets with unintended default values.
Prevention: Future audio handlers must wrap TryGet*Field checks in a HasField guard to properly return malformed input errors when a field is present but has the incorrect type.
